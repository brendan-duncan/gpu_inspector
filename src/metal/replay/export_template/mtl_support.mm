#include "mtl_support.h"

#include "frame_window.h"

#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

id<MTLDevice> device = nil;
id<MTLCommandQueue> queue = nil;

// The window
static FrameWindow* outputWindow = nullptr;
static CAMetalLayer* outputLayer = nil;
static bool outputVsync = true;
static std::string outputTitle;
static uint64_t framesShown = 0;
static uint64_t framesAtTitle = 0;
static std::chrono::steady_clock::time_point titleTime;
static bool OutputWindowOpen(void) { return outputWindow != nullptr; }

namespace
{

std::vector<uint8_t> g_data;

struct Comparison
{
    std::string name;
    id<MTLBuffer> staging;
    id<MTLTexture> texture;
    uint64_t width, height, rowBytes;
    const uint8_t* captured;
    uint64_t capturedSize;
};
std::vector<Comparison> g_comparisons;

// ---------------------------------------------------------------------------------------------
// PNG, with no dependency: stored deflate blocks and a CRC, which is all an 8-bit RGBA image needs.

uint32_t Crc32(const uint8_t* data, size_t size)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

bool WritePng(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba)
{
    std::vector<uint8_t> raw;
    raw.reserve(((size_t)width * 4 + 1) * height);
    for (uint32_t y = 0; y < height; ++y)
    {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + (size_t)y * width * 4, rgba.begin() + ((size_t)y + 1) * width * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t pos = 0; pos < raw.size();)
    {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF));
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF));
        z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw)
    {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8)
        z.push_back((uint8_t)(adler >> s));
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.write((const char*)signature, 8);
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        std::vector<uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        const uint32_t length = (uint32_t)data.size();
        for (int s = 24; s >= 0; s -= 8)
            out.put((char)(length >> s));
        out.write((const char*)body.data(), (std::streamsize)body.size());
        const uint32_t crc = Crc32(body.data(), body.size());
        for (int s = 24; s >= 0; s -= 8)
            out.put((char)(crc >> s));
    };
    std::vector<uint8_t> header;
    for (int s = 24; s >= 0; s -= 8)
        header.push_back((uint8_t)(width >> s));
    for (int s = 24; s >= 0; s -= 8)
        header.push_back((uint8_t)(height >> s));
    header.insert(header.end(), {8, 6, 0, 0, 0});   // 8-bit RGBA
    chunk("IHDR", header);
    chunk("IDAT", z);
    chunk("IEND", {});
    return (bool)out;
}

/** One subresource as 8-bit RGBA, for the PNG; false for a format with no obvious mapping. */
bool ToRgba(const Comparison& c, const uint8_t* bytes, size_t size, std::vector<uint8_t>& out)
{
    const MTLPixelFormat format = c.texture.pixelFormat;
    const uint64_t texels = c.width * c.height;
    out.assign((size_t)texels * 4, 0);
    auto rows = [&](size_t texelBytes, void (*convert)(const uint8_t*, uint8_t*)) {
        for (uint64_t y = 0; y < c.height; ++y)
        {
            const uint8_t* row = bytes + y * c.rowBytes;
            for (uint64_t x = 0; x < c.width; ++x)
            {
                if ((y * c.rowBytes) + (x + 1) * texelBytes > size)
                    return;
                convert(row + x * texelBytes, out.data() + ((y * c.width) + x) * 4);
            }
        }
    };
    switch (format)
    {
        case MTLPixelFormatBGRA8Unorm:
        case MTLPixelFormatBGRA8Unorm_sRGB:
            rows(4, [](const uint8_t* in, uint8_t* p) { p[0] = in[2]; p[1] = in[1]; p[2] = in[0]; p[3] = in[3]; });
            return true;
        case MTLPixelFormatRGBA8Unorm:
        case MTLPixelFormatRGBA8Unorm_sRGB:
            rows(4, [](const uint8_t* in, uint8_t* p) { std::memcpy(p, in, 4); });
            return true;
        case MTLPixelFormatR8Unorm:
            rows(1, [](const uint8_t* in, uint8_t* p) { p[0] = p[1] = p[2] = in[0]; p[3] = 255; });
            return true;
        case MTLPixelFormatDepth32Float:
        case MTLPixelFormatDepth32Float_Stencil8:
            rows(4, [](const uint8_t* in, uint8_t* p) {
                float d = 0;
                std::memcpy(&d, in, 4);
                const uint8_t g = (uint8_t)std::min(255.0f, std::max(0.0f, d * 255.0f));
                p[0] = p[1] = p[2] = g;
                p[3] = 255;
            });
            return true;
        default:
            return false;
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// The device and the data file

void Fail(const char* what, NSError* error)
{
    std::fprintf(stderr, "%s failed: %s\n", what,
        error ? error.localizedDescription.UTF8String : "no error given");
    std::exit(2);
}

void Fail(const std::string& message)
{
    std::fprintf(stderr, "%s\n", message.c_str());
    std::exit(2);
}

void CreateDevice(void)
{
    device = MTLCreateSystemDefaultDevice();
    if (!device)
        Fail("no Metal device on this machine");
    queue = [device newCommandQueue];
    queue.label = @"exported frame";
}

const char* DeviceName(void) { return device ? device.name.UTF8String : "none"; }

bool LoadData(const std::string& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    const std::streamsize size = in.tellg();
    in.seekg(0);
    g_data.resize((size_t)size);
    return (bool)in.read((char*)g_data.data(), size);
}

const void* Data(uint64_t offset, uint64_t size)
{
    if (offset + size > g_data.size())
        Fail("the data file is shorter than the frame expects");
    return g_data.data() + offset;
}

NSString* LoadShader(NSString* path)
{
    NSString* beside = [[[NSBundle mainBundle] executablePath] stringByDeletingLastPathComponent];
    for (NSString* candidate in @[ path, [beside stringByAppendingPathComponent:path] ])
    {
        NSString* source = [NSString stringWithContentsOfFile:candidate encoding:NSUTF8StringEncoding error:nil];
        if (source)
            return source;
    }
    Fail(std::string("could not read ") + path.UTF8String);
}

id<MTLLibrary> LoadLibrary(const void* bytes, uint64_t size, NSError** error)
{
    if (!bytes || !size)
        return nil;
    dispatch_data_t data = dispatch_data_create(bytes, (size_t)size, dispatch_get_main_queue(),
        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    return [device newLibraryWithData:data error:error];
}

// ---------------------------------------------------------------------------------------------
// Uploads

void UploadTexture(id<MTLTexture> texture, const void* data, uint64_t size, uint64_t rowBytes,
    uint64_t width, uint64_t height, uint64_t slice, uint64_t level)
{
    if (!texture || !data || !size)
        return;
    id<MTLBuffer> staging = [device newBufferWithBytes:data
                                                length:(NSUInteger)size
                                               options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromBuffer:staging
               sourceOffset:0
          sourceBytesPerRow:(NSUInteger)rowBytes
        sourceBytesPerImage:(NSUInteger)size
                 sourceSize:MTLSizeMake((NSUInteger)width, (NSUInteger)height, 1)
                  toTexture:texture
           destinationSlice:(NSUInteger)slice
           destinationLevel:(NSUInteger)level
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
}

void UploadBuffer(id<MTLBuffer> buffer, uint64_t offset, const void* data, uint64_t size)
{
    if (!buffer || !data || !size)
        return;
    if (offset + size > buffer.length)
        Fail("a buffer upload does not fit the buffer");
    if (buffer.storageMode != MTLStorageModePrivate)
    {
        std::memcpy((uint8_t*)buffer.contents + offset, data, (size_t)size);
        if (buffer.storageMode == MTLStorageModeManaged)
        {
            [buffer didModifyRange:NSMakeRange((NSUInteger)offset, (NSUInteger)size)];
        }
        return;
    }
    id<MTLBuffer> staging = [device newBufferWithBytes:data
                                                length:(NSUInteger)size
                                               options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromBuffer:staging
             sourceOffset:0
                 toBuffer:buffer
        destinationOffset:(NSUInteger)offset
                     size:(NSUInteger)size];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
}

// ---------------------------------------------------------------------------------------------
// Read-back and comparison

void ReadbackTexture(id<MTLCommandBuffer> commands, id<MTLTexture> texture, const char* name,
    uint64_t slice, uint64_t level, uint64_t width, uint64_t height, uint64_t rowBytes,
    uint64_t imageBytes, MTLBlitOption options, const void* captured, uint64_t capturedSize)
{
    if (!texture || OutputWindowOpen())
        return;   // shown, not compared: the comparison is --batch's
    id<MTLBuffer> staging = [device newBufferWithLength:(NSUInteger)imageBytes
                                                options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    blit.label = @"read-back";
    [blit copyFromTexture:texture
                     sourceSlice:(NSUInteger)slice
                     sourceLevel:(NSUInteger)level
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake((NSUInteger)width, (NSUInteger)height, 1)
                        toBuffer:staging
               destinationOffset:0
          destinationBytesPerRow:(NSUInteger)rowBytes
        destinationBytesPerImage:(NSUInteger)imageBytes
                         options:options];
    [blit endEncoding];
    g_comparisons.push_back({name, staging, texture, width, height, rowBytes, (const uint8_t*)captured, capturedSize});
}

int ReportResults(const std::string& directory, bool writeImages)
{
    if (writeImages && !g_comparisons.empty())
    {
        [[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:directory.c_str()]
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
    }
    size_t identical = 0, differing = 0, skipped = 0;
    for (const Comparison& c : g_comparisons)
    {
        std::printf("%s (%llux%llu): ", c.name.c_str(), (unsigned long long)c.width, (unsigned long long)c.height);
        if (!c.captured || !c.capturedSize)
        {
            std::printf("the capture holds no copy to compare with\n");
            ++skipped;
            continue;
        }
        const uint8_t* replayed = (const uint8_t*)c.staging.contents;
        const size_t size = std::min<size_t>((size_t)c.capturedSize, (size_t)c.staging.length);
        const uint64_t texels = c.width * c.height;
        const size_t texelBytes = texels ? std::max<size_t>(1, size / (size_t)texels) : 1;
        uint64_t differingTexels = 0;
        uint32_t maxDelta = 0;
        for (size_t i = 0; i < size; i += texelBytes)
        {
            const size_t width = std::min(texelBytes, size - i);
            if (std::memcmp(replayed + i, c.captured + i, width) == 0)
                continue;
            ++differingTexels;
            for (size_t b = i; b < i + width; ++b)
            {
                maxDelta = std::max<uint32_t>(maxDelta, (uint32_t)std::abs((int)replayed[b] - (int)c.captured[b]));
            }
        }
        if (!differingTexels)
        {
            std::printf("identical\n");
            ++identical;
        }
        else
        {
            std::printf("%llu of %llu texels differ, largest byte delta %u\n",
                (unsigned long long)differingTexels, (unsigned long long)texels, maxDelta);
            ++differing;
        }
        if (!writeImages)
            continue;
        std::string file = c.name;
        for (char& ch : file)
            if (ch == ' ' || ch == '/')
                ch = '_';
        const std::string base = directory + "/" + file;
        std::vector<uint8_t> a, b;
        if (ToRgba(c, c.captured, (size_t)c.capturedSize, a) && ToRgba(c, replayed, size, b))
        {
            WritePng(base + "_captured.png", (uint32_t)c.width, (uint32_t)c.height, a);
            WritePng(base + "_replayed.png", (uint32_t)c.width, (uint32_t)c.height, b);
        }
    }
    if (writeImages && !g_comparisons.empty())
        std::printf("wrote the targets to %s/\n", directory.c_str());
    std::printf("%zu identical, %zu differing, %zu not compared\n", identical, differing, skipped);
    return differing == 0 && skipped == 0 ? 0 : 1;
}

// ---- The window

void SetOutputVsync(bool on) { outputVsync = on; }

bool OpenOutputWindow(id<MTLTexture> output, const char* title)
{
    if (!output)
    {
        std::fprintf(stderr, "the frame has no output to show\n");
        return false;
    }
    // A layer's drawables come in a handful of formats, and the output is copied, not converted.
    bool showable = output.textureType == MTLTextureType2D && output.sampleCount <= 1;
    switch (output.pixelFormat)
    {
        case MTLPixelFormatBGRA8Unorm:
        case MTLPixelFormatBGRA8Unorm_sRGB:
        case MTLPixelFormatRGBA16Float:
        case MTLPixelFormatRGB10A2Unorm:
        case MTLPixelFormatBGR10A2Unorm:
            break;
        default:
            showable = false;
    }
    if (!showable)
    {
        std::fprintf(stderr, "the frame's output (pixel format %lu) is not something a window's layer can show\n", (unsigned long)output.pixelFormat);
        return false;
    }
    outputWindow = OpenFrameWindow(title, (uint32_t)output.width, (uint32_t)output.height);
    if (!outputWindow)
    {
        std::fprintf(stderr, "no window could be opened\n");
        return false;
    }
    outputLayer = (__bridge CAMetalLayer*)FrameWindowHandle(outputWindow);
    outputLayer.device = device;
    outputLayer.pixelFormat = output.pixelFormat;
    outputLayer.framebufferOnly = NO;   // the output is blitted into the drawable
    outputLayer.drawableSize = CGSizeMake(output.width, output.height);
    outputLayer.displaySyncEnabled = outputVsync;
    outputTitle = title ? title : "";
    titleTime = std::chrono::steady_clock::now();
    return true;
}

bool PresentOutput(id<MTLTexture> output)
{
    if (!outputWindow)
        return false;
    @autoreleasepool
    {
        if (!PumpFrameWindow(outputWindow))
            return false;
        id<CAMetalDrawable> drawable = [outputLayer nextDrawable];
        if (drawable)
        {
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            [blit copyFromTexture:output
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(output.width, output.height, 1)
                        toTexture:drawable.texture
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            [commands presentDrawable:drawable];
            [commands commit];
        }
    }
    // The frame rate in the title, twice a second.
    ++framesShown;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - titleTime).count();
    if (seconds >= 0.5)
    {
        char text[320];
        std::snprintf(text, sizeof(text), "%s - %.1f fps", outputTitle.c_str(), (double)(framesShown - framesAtTitle) / seconds);
        SetFrameWindowTitle(outputWindow, text);
        framesAtTitle = framesShown;
        titleTime = now;
    }
    return true;
}

int CloseOutputWindow(void)
{
    std::printf("frames shown: %llu\n", (unsigned long long)framesShown);
    outputLayer = nil;
    if (outputWindow)
        CloseFrameWindow(outputWindow);
    outputWindow = nullptr;
    return 0;
}
