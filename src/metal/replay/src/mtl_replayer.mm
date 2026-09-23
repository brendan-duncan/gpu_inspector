#include "mtl_replayer.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "decode.h"   // vkreplay::DecodeBase64

#include "formats.h"  // mtlinsp::PixelFormatDetails, DepthReadbackDetails

#include "mtl_exporter.h"
#include "mtl_raytracing.h"
#include "mtl_reflect.h"

namespace mtlreplay
{
namespace
{

std::string Text(const JValue* v) { return v && v->IsString() ? std::string(v->Str()) : std::string(); }

/** The variable stem an object of this type is named after in the exported project. */
const char* StemOf(const std::string& type)
{
    if (type == "MTLTexture")
        return "texture";
    if (type == "MTLBuffer")
        return "buffer";
    if (type == "MTLLibrary")
        return "library";
    if (type == "MTLFunction")
        return "function";
    if (type == "MTLRenderPipelineState")
        return "pipeline";
    if (type == "MTLComputePipelineState")
        return "compute_pipeline";
    if (type == "MTLDepthStencilState")
        return "depth_stencil";
    if (type == "MTLSamplerState")
        return "sampler";
    if (type == "MTLHeap")
        return "heap";
    if (type == "MTLFence")
        return "fence";
    if (type == "MTLEvent" || type == "MTLSharedEvent")
        return "event";
    if (type == "MTLIndirectCommandBuffer")
        return "icb";
    if (type == "MTLAccelerationStructure")
        return "acceleration_structure";
    if (type == "MTLCommandQueue")
        return "queue";
    return "object";
}

/** The Objective-C type a global of this kind is declared with. */
const char* TypeOf(const std::string& type)
{
    if (type == "MTLTexture")
        return "id<MTLTexture>";
    if (type == "MTLBuffer")
        return "id<MTLBuffer>";
    if (type == "MTLLibrary")
        return "id<MTLLibrary>";
    if (type == "MTLFunction")
        return "id<MTLFunction>";
    if (type == "MTLRenderPipelineState")
        return "id<MTLRenderPipelineState>";
    if (type == "MTLComputePipelineState")
        return "id<MTLComputePipelineState>";
    if (type == "MTLDepthStencilState")
        return "id<MTLDepthStencilState>";
    if (type == "MTLSamplerState")
        return "id<MTLSamplerState>";
    if (type == "MTLHeap")
        return "id<MTLHeap>";
    if (type == "MTLFence")
        return "id<MTLFence>";
    if (type == "MTLEvent")
        return "id<MTLEvent>";
    if (type == "MTLSharedEvent")
        return "id<MTLSharedEvent>";
    if (type == "MTLIndirectCommandBuffer")
        return "id<MTLIndirectCommandBuffer>";
    if (type == "MTLAccelerationStructure")
        return "id<MTLAccelerationStructure>";
    return "id";
}

/** The data type a function constant of this name holds, and how many components. */
bool ConstantType(const std::string& name, MTLDataType* type, uint32_t* count)
{
    static const struct
    {
        const char* base;
        MTLDataType first;
    } kFamilies[] = {
        {"float", MTLDataTypeFloat},
        {"half", MTLDataTypeHalf},
        {"int", MTLDataTypeInt},
        {"uint", MTLDataTypeUInt},
        {"short", MTLDataTypeShort},
        {"ushort", MTLDataTypeUShort},
        {"char", MTLDataTypeChar},
        {"uchar", MTLDataTypeUChar},
        {"bool", MTLDataTypeBool},
    };
    for (const auto& f : kFamilies)
    {
        const size_t length = std::strlen(f.base);
        if (name.compare(0, length, f.base) != 0)
            continue;
        const std::string rest = name.substr(length);
        uint32_t components = 1;
        if (!rest.empty())
        {
            if (rest.size() != 1 || rest[0] < '2' || rest[0] > '4')
                continue;
            components = (uint32_t)(rest[0] - '0');
        }
        *type = (MTLDataType)((NSUInteger)f.first + components - 1);
        *count = components;
        return true;
    }
    return false;
}

} // namespace

MtlReplayer::MtlReplayer(const CaptureFile& capture, MtlReplayOptions options)
    : _capture(capture), _options(std::move(options)) {}

MtlReplayer::~MtlReplayer() { delete _x; }

void MtlReplayer::Problem(const std::string& message)
{
    if (_env.problems.size() < 10000)
        _env.problems.push_back(message);
}

void MtlReplayer::LeftOut(uint32_t index, const std::string& method, const std::string& why)
{
    ++_leftOut;
    Problem("command " + std::to_string(index) + " (" + method + ") left out: " + why);
    if (_x)
        _x->LeftOut(index, method, why);
}

id MtlReplayer::Object(uint64_t id) const
{
    const auto it = _objects.find(id);
    return it == _objects.end() ? nil : it->second;
}

const JValue* MtlReplayer::Record(uint64_t id) const
{
    const auto it = _records.find(id);
    return it == _records.end() ? nullptr : it->second;
}

std::string MtlReplayer::LabelOf(uint64_t id) const
{
    const JValue* record = Record(id);
    const JValue* label = record ? record->Get("label") : nullptr;
    return label && label->IsString() ? std::string(label->Str()) : std::string();
}

// ------------------------------------------------------------------------------------------
// Running

bool MtlReplayer::Run(MtlReplayReport& report)
{
    @autoreleasepool
    {
        _device = MTLCreateSystemDefaultDevice();
        if (_device == nil)
        {
            report.problems.push_back("no Metal device on this machine");
            return false;
        }
        _queue = [_device newCommandQueue];
        _queue.label = @"mtlinsp_replay";
        report.device = _device.name.UTF8String;

        _env.object = [this](uint64_t id) { return Object(id); };

        // What the capture holds, indexed before anything is created.
        if (const JValue* objects = _capture.Objects())
        {
            for (uint32_t i = 0; i < objects->count; ++i)
            {
                const JValue& o = objects->items[i];
                const JValue* id = o.Get("id");
                if (id)
                    _records[id->Uint()] = &o;
                if (Text(o.Get("type")) == "MTLDevice")
                    report.capturedDevice = Text(o.Get("args") ? o.Get("args")->Get("name") : nullptr);
            }
        }
        if (const JValue* buffers = _capture.Buffers())
        {
            for (uint32_t i = 0; i < buffers->count; ++i)
            {
                const JValue* info = buffers->items[i].Get("info");
                if (info && info->Get("id"))
                    _bufferData[info->Get("id")->Uint()] = &buffers->items[i];
            }
        }
        if (const JValue* textures = _capture.Textures())
        {
            for (uint32_t i = 0; i < textures->count; ++i)
            {
                const JValue* info = textures->items[i].Get("info");
                if (!info)
                    continue;
                const std::string kind = Text(info->Get("kind"));
                if (kind == "sampled" || kind == "initial")
                {
                    if (info->Get("capture"))
                        _textureData[info->Get("capture")->Uint()] = &textures->items[i];
                }
                else if (_options.compareTargets)
                {
                    const uint64_t cb = info->Get("commandBuffer") ? info->Get("commandBuffer")->Uint() : 0;
                    const uint32_t pass = info->Get("passIndex") ? (uint32_t)info->Get("passIndex")->Uint() : 0;
                    _targets[{cb, pass}].push_back(&textures->items[i]);
                }
            }
        }

        if (!_options.exportDir.empty())
        {
            _x = new MtlExporter(_options.exportDir, _capture);
            std::string error;
            if (!_x->Open(error))
            {
                report.exported.directory = _options.exportDir;
                report.exported.error = error;
                delete _x;
                _x = nullptr;
            }
            else
            {
                _x->Alias(_device, "device");
                _x->Alias(_queue, "queue");
                _x->Device(report.capturedDevice, report.device);
            }
        }

        CreateObjects();
        // A function table's entries and buffers, now that everything it names exists.
        FillFunctionTables();
        UploadContents();
        ReplayCommands();
        // The frame's storage textures, which have no pass end to be read back at.
        CompareWrittenTextures(report);
        CompleteReadbacks(report);

        report.objects = _objectCount;
        report.commands = _commandCount;
        report.submissions = _submissionCount;
        for (const std::string& p : _env.problems)
            report.problems.push_back(p);
        if (_env.unresolved)
        {
            report.problems.push_back(std::to_string(_env.unresolved) +
                " references named an object the replay does not have");
        }
        if (_x)
        {
            if (id output = _lastDrawable ? Object(_lastDrawable) : nil)
                _x->FrameOutput(_x->NameOf(output), _lastDrawable);
            _x->Finish(report, report.exported);
            delete _x;
            _x = nullptr;
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------------
// The object graph

void MtlReplayer::CreateObjects()
{
    const JValue* objects = _capture.Objects();
    if (!objects)
        return;
    if (_x)
        _x->Comment(MtlExporter::Create, "The capture's objects, in the order it created them.");
    for (uint32_t i = 0; i < objects->count; ++i)
    {
        @autoreleasepool
        {
            CreateObject(objects->items[i]);
        }
    }
}

void MtlReplayer::CreateObject(const JValue& object)
{
    const JValue* idValue = object.Get("id");
    if (!idValue)
        return;
    const uint64_t captureId = idValue->Uint();
    const std::string type = Text(object.Get("type"));
    if (_options.trace)
        std::fprintf(stderr, "object %" PRIu64 " %s\n", captureId, type.c_str());
    // The capture's device and its queues are the replay's own, which are already made.
    if (type == "MTLDevice")
    {
        _objects[captureId] = _device;
        return;
    }
    if (type == "MTLCommandQueue")
    {
        _objects[captureId] = _queue;
        return;
    }
    if (type == "MTLCommandBuffer")
        return;   // made as the frame is replayed

    _env.where = type + " " + std::to_string(captureId);
    const Decoder d(object.Get("args"), _env);
    id made = nil;
    // Every maker that fails says why — a shader that would not compile, a parent that is not in
    // the replay. So the catch-all below only speaks when none of them did, which is what this
    // count tells it: a maker that returned nil silently is the one case with nothing said yet.
    const size_t problemsBefore = _env.problems.size();
    if (type == "MTLTexture")
        made = CreateTexture(object, d, captureId);
    else if (type == "MTLBuffer")
        made = CreateBuffer(object, d, captureId);
    else if (type == "MTLLibrary")
        made = CreateLibrary(object, d, captureId);
    else if (type == "MTLFunction")
        made = CreateFunction(object, d, captureId);
    else if (type == "MTLRenderPipelineState")
        made = CreateRenderPipeline(object, d, captureId);
    else if (type == "MTLComputePipelineState")
        made = CreateComputePipeline(object, d, captureId);
    else if (type == "MTLDepthStencilState")
        made = CreateDepthStencil(object, d, captureId);
    else if (type == "MTLSamplerState")
        made = CreateSampler(object, d, captureId);
    else if (type == "MTLHeap")
        made = CreateHeap(object, d, captureId);
    else if (type == "MTLAccelerationStructure")
        made = CreateAccelerationStructure(object, d, captureId);
    else if (type == "MTLIntersectionFunctionTable")
        made = CreateFunctionTable(object, d, captureId, true);
    else if (type == "MTLVisibleFunctionTable")
        made = CreateFunctionTable(object, d, captureId, false);
    else if (type == "MTLFence")
    {
        made = [_device newFence];
        if (_x && made)
        {
            const std::string name = _x->Declare(TypeOf(type), StemOf(type), captureId, made);
            _x->Block(MtlExporter::Create, "", [&](Source& w) { w.Line(name + " = [device newFence];"); });
        }
    }
    else if (type == "MTLEvent" || type == "MTLSharedEvent")
    {
        made = type == "MTLEvent" ? (id)[_device newEvent] : (id)[_device newSharedEvent];
        if (_x && made)
        {
            const std::string name = _x->Declare(TypeOf(type), StemOf(type), captureId, made);
            const std::string call = type == "MTLEvent" ? "newEvent" : "newSharedEvent";
            _x->Block(MtlExporter::Create, "", [&](Source& w) { w.Line(name + " = [device " + call + "];"); });
        }
    }
    else
    {
        // Everything the replay has no maker for yet: argument encoders and indirect command
        // buffers. Named as a problem so the report says what the frame will miss,
        // and `reported` so the catch-all below does not say the same thing a second way.
        Problem("no maker for " + type + " " + std::to_string(captureId) +
            " (" + Text(object.Get("cmd")) + ")");
    }
    _env.where.clear();
    if (made)
    {
        _objects[captureId] = made;
        ++_objectCount;
        if (_x)
            _x->CountObject();
    }
    else if (_env.problems.size() == problemsBefore)
    {
        Problem(type + " " + std::to_string(captureId) + " (" + Text(object.Get("cmd")) + ") was not created");
    }
}

id<MTLTexture> MtlReplayer::CreateTexture(const JValue& object, const Decoder& d, uint64_t captureId)
{
    const std::string cmd = Text(object.Get("cmd"));
    const bool drawable = d.Bool("drawable");
    if (drawable)
    {
        _drawables.insert(captureId);
        _lastDrawable = captureId;
    }

    // A texture view names its parent and the format it reinterprets it as.
    if (d.Bool("view"))
    {
        id<MTLTexture> parent = d.Object("parentTexture");
        if (!parent)
        {
            Problem("texture view " + std::to_string(captureId) + " has no parent in the replay");
            return nil;
        }
        const MTLPixelFormat format = (MTLPixelFormat)d.Enum("pixelFormat", MTL_TABLE(MTLPixelFormat));
        const MTLTextureType textureType = (MTLTextureType)d.Enum("textureType", MTL_TABLE(MTLTextureType));
        const NSRange levels = d.Range("levels");
        const NSRange slices = d.Range("slices");
        id<MTLTexture> view = levels.length || slices.length
            ? [parent newTextureViewWithPixelFormat:format textureType:textureType levels:levels slices:slices]
            : [parent newTextureViewWithPixelFormat:format];
        if (view && _x)
        {
            const std::string name = _x->Declare("id<MTLTexture>", "texture", captureId, view);
            const std::string parentName = _x->NameOf(parent);
            _x->Block(MtlExporter::Create, LabelOf(captureId), [&](Source& w) {
                if (levels.length || slices.length)
                {
                    w.Line(name + " = [" + parentName + " newTextureViewWithPixelFormat:" +
                        Source::Enum(MTL_TABLE(MTLPixelFormat), format) + " textureType:" +
                        Source::Enum(MTL_TABLE(MTLTextureType), textureType) + " levels:NSMakeRange(" +
                        Source::Uint(levels.location) + ", " + Source::Uint(levels.length) + ") slices:NSMakeRange(" +
                        Source::Uint(slices.location) + ", " + Source::Uint(slices.length) + ")];");
                }
                else
                {
                    w.Line(name + " = [" + parentName + " newTextureViewWithPixelFormat:" +
                        Source::Enum(MTL_TABLE(MTLPixelFormat), format) + "];");
                }
            });
        }
        return view;
    }

    MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
    MTLTextureDescriptor* defaults = [[MTLTextureDescriptor alloc] init];
    FillVisitor fill(d);
    Reflect(fill, descriptor, defaults);

    if (drawable)
    {
        // There is no window here, so the drawable's texture becomes an ordinary render target.
        // The capture records a drawable's properties rather than a descriptor, and `usage` is a
        // raw number; what it must have either way is RenderTarget, since the frame draws into it.
        descriptor.usage |= MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModePrivate;
        if (_x)
            _x->Note("the drawable's texture is an ordinary render target here: an exported frame has no window");
    }
    // Every texture the replay might read back has to be copyable, and a capture only records the
    // usage the application asked for.
    if (descriptor.storageMode == MTLStorageModeMemoryless)
        descriptor.storageMode = MTLStorageModePrivate;

    id<MTLTexture> texture = nil;
    id<MTLHeap> heap = d.Object("heap");
    if (heap && cmd.compare(0, 4, "heap") == 0)
    {
        texture = [heap newTextureWithDescriptor:descriptor offset:d.Uint("heapOffset")];
    }
    if (!texture)
        texture = [_device newTextureWithDescriptor:descriptor];
    if (!texture)
        return nil;
    texture.label = [NSString stringWithUTF8String:LabelOf(captureId).c_str()];

    if (_x)
    {
        const std::string name = _x->Declare("id<MTLTexture>", "texture", captureId, texture);
        _x->Block(MtlExporter::Create, LabelOf(captureId), [&](Source& w) {
            const std::string var = w.Local("descriptor");
            w.Line("MTLTextureDescriptor* " + var + " = [[MTLTextureDescriptor alloc] init];");
            EmitVisitor emit(w, var + ".");
            Reflect(emit, descriptor, defaults);
            w.Line(name + " = [device newTextureWithDescriptor:" + var + "];");
            const std::string label = LabelOf(captureId);
            if (!label.empty())
                w.Line(name + ".label = " + Source::NSString(label, true) + ";");
        });
    }
    return texture;
}

id<MTLBuffer> MtlReplayer::CreateBuffer(const JValue& object, const Decoder& d, uint64_t captureId)
{
    const uint64_t length = d.Uint("length");
    if (!length)
    {
        Problem("buffer " + std::to_string(captureId) + " has no length");
        return nil;
    }
    // Storage: what the capture recorded, except that a private buffer is made shared here so its
    // contents can be written without a staging blit, and read if an analysis wants them. Metal
    // does not let a buffer's storage change what a shader sees.
    MTLResourceOptions options = (MTLResourceOptions)d.Uint("options");
    const MTLStorageMode storage =
        (MTLStorageMode)((options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift);
    if (storage == MTLStorageModeMemoryless)
    {
        options = (options & ~MTLResourceStorageModeMask) | MTLResourceStorageModePrivate;
    }
    id<MTLBuffer> buffer = nil;
    id<MTLHeap> heap = d.Object("heap");
    if (heap)
        buffer = [heap newBufferWithLength:(NSUInteger)length options:options];
    if (!buffer)
        buffer = [_device newBufferWithLength:(NSUInteger)length options:options];
    if (!buffer)
        return nil;
    const std::string label = LabelOf(captureId);
    if (!label.empty())
        buffer.label = [NSString stringWithUTF8String:label.c_str()];

    if (_x)
    {
        const std::string name = _x->Declare("id<MTLBuffer>", "buffer", captureId, buffer);
        _x->Block(MtlExporter::Create, label, [&](Source& w) {
            w.Line(name + " = [device newBufferWithLength:" + Source::Uint(length) +
                " options:" + Source::Flags(MTL_TABLE(MTLResourceOptions), (uint64_t)options) + "];");
            if (!label.empty())
                w.Line(name + ".label = " + Source::NSString(label, true) + ";");
        });
    }
    return buffer;
}

id<MTLLibrary> MtlReplayer::CreateLibrary(const JValue& object, const Decoder& d, uint64_t captureId)
{
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    NSError* error = nil;
    id<MTLLibrary> library = nil;
    std::string shaderFile;

    // Source first: it is what a bug report wants to read, and what the exported project compiles.
    if (_capture.Blob(object, "Metal Shading Language", bytes, size) && size)
    {
        NSString* source = [[NSString alloc] initWithBytes:bytes length:size encoding:NSUTF8StringEncoding];
        library = [_device newLibraryWithSource:source options:nil error:&error];
        if (!library)
        {
            Problem("library " + std::to_string(captureId) + " did not compile: " +
                (error ? error.localizedDescription.UTF8String : "no error given"));
            return nil;
        }
        if (_x)
            shaderFile = _x->Shader("library_" + std::to_string(captureId),
                std::string_view((const char*)bytes, size));
    }
    else if (_capture.Blob(object, "metallib", bytes, size) && size)
    {
        dispatch_data_t data = dispatch_data_create(bytes, size, dispatch_get_main_queue(),
            DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        library = [_device newLibraryWithData:data error:&error];
        if (!library)
        {
            Problem("library " + std::to_string(captureId) + " did not load: " +
                (error ? error.localizedDescription.UTF8String : "no error given"));
            return nil;
        }
    }
    else
    {
        Problem("library " + std::to_string(captureId) + " (" + Text(object.Get("cmd")) +
            ") has neither source nor metallib in the capture, so it cannot be re-created");
        return nil;
    }

    if (_x)
    {
        const std::string name = _x->Declare("id<MTLLibrary>", "library", captureId, library);
        _x->Block(MtlExporter::Create, LabelOf(captureId), [&](Source& w) {
            const std::string err = w.Local("error");
            w.Line("NSError* " + err + " = nil;");
            if (!shaderFile.empty())
            {
                w.Line(name + " = [device newLibraryWithSource:LoadShader(@\"" + shaderFile +
                    "\") options:nil error:&" + err + "];");
            }
            else
            {
                w.Line(name + " = LoadLibrary(" + _x->Data(bytes, size) + ", " + Source::Uint(size) +
                    ", &" + err + ");");
            }
            w.Line("if (!" + name + ") Fail(\"library " + std::to_string(captureId) + "\", " + err + ");");
        });
    }
    return library;
}

id<MTLFunction> MtlReplayer::CreateFunction(const JValue& object, const Decoder& d, uint64_t captureId)
{
    const uint64_t parent = object.Get("parent") ? object.Get("parent")->Uint() : 0;
    id<MTLLibrary> library = (id<MTLLibrary>)Object(parent);
    if (!library)
    {
        Problem("function " + std::to_string(captureId) + " has no library in the replay");
        return nil;
    }
    NSString* fname = d.NSStr("name");
    if (!fname)
    {
        Problem("function " + std::to_string(captureId) + " has no name");
        return nil;
    }

    // A specialized function: the constants the application set, which the capture watched the
    // setters of (src/metal/src/function_constants.h) because Metal will not say.
    const JValue* constants = d.Get("constantValues");
    MTLFunctionConstantValues* values = nil;
    if (constants && constants->IsArray() && constants->count)
    {
        values = [[MTLFunctionConstantValues alloc] init];
        for (uint32_t i = 0; i < constants->count; ++i)
        {
            const Decoder c(&constants->items[i], _env);
            MTLDataType type = MTLDataTypeNone;
            uint32_t count = 0;
            if (!ConstantType(c.Str("type"), &type, &count))
                continue;
            // Widest component first: every scalar family fits in eight bytes per component.
            uint8_t storage[4 * 8] = {};
            const JValue* value = c.Get("value");
            for (uint32_t n = 0; n < count; ++n)
            {
                const JValue* component = value && value->IsArray() ? (n < value->count ? &value->items[n] : nullptr)
                                                                    : (n == 0 ? value : nullptr);
                if (!component)
                    continue;
                const std::string base = c.Str("type");
                if (base.compare(0, 5, "float") == 0)
                {
                    const float f = (float)component->Double();
                    std::memcpy(storage + n * 4, &f, 4);
                }
                else if (base.compare(0, 4, "half") == 0)
                {
                    const __fp16 h = (__fp16)component->Double();
                    std::memcpy(storage + n * 2, &h, 2);
                }
                else if (base.compare(0, 4, "bool") == 0)
                {
                    storage[n] = component->IsBool() ? (component->boolean ? 1 : 0) : (component->Uint() ? 1 : 0);
                }
                else
                {
                    const uint64_t raw = (uint64_t)component->Int();
                    const size_t width = base.compare(0, 5, "short") == 0 || base.compare(0, 6, "ushort") == 0 ? 2
                        : base.compare(0, 4, "char") == 0 || base.compare(0, 5, "uchar") == 0                  ? 1
                                                                                                               : 4;
                    std::memcpy(storage + n * width, &raw, width);
                }
            }
            if (c.Has("index"))
            {
                [values setConstantValue:storage type:type atIndex:(NSUInteger)c.Uint("index")];
            }
            else if (NSString* cname = c.NSStr("name"))
            {
                [values setConstantValue:storage type:type withName:cname];
            }
        }
    }

    NSError* error = nil;
    id<MTLFunction> function = values ? [library newFunctionWithName:fname constantValues:values error:&error]
                                      : [library newFunctionWithName:fname];
    if (!function)
    {
        Problem("function " + std::string(fname.UTF8String) + " (" + std::to_string(captureId) + ") was not made: " +
            (error ? error.localizedDescription.UTF8String : "the library has no such function"));
        return nil;
    }
    if (_x)
    {
        const std::string name = _x->Declare("id<MTLFunction>", "function", captureId, function);
        const std::string libraryName = _x->NameOf(library);
        // The specialized form is written from the values object, so the exported program
        // specializes the same way rather than taking the constants' defaults.
        const bool specialized = values != nil;
        _x->Block(MtlExporter::Create, std::string(fname.UTF8String), [&](Source& w) {
            if (!specialized)
            {
                w.Line(name + " = [" + libraryName + " newFunctionWithName:" +
                    Source::NSString(fname.UTF8String, true) + "];");
                return;
            }
            const std::string var = w.Local("constants");
            w.Line("MTLFunctionConstantValues* " + var + " = [[MTLFunctionConstantValues alloc] init];");
            for (uint32_t i = 0; i < constants->count; ++i)
            {
                const Decoder c(&constants->items[i], _env);
                MTLDataType type = MTLDataTypeNone;
                uint32_t count = 0;
                const std::string base = c.Str("type");
                if (!ConstantType(base, &type, &count))
                    continue;
                const std::string slot = w.Local("constant");
                const JValue* value = c.Get("value");
                std::string items;
                for (uint32_t n = 0; n < count; ++n)
                {
                    const JValue* component = value && value->IsArray() ? (n < value->count ? &value->items[n] : nullptr)
                                                                        : (n == 0 ? value : nullptr);
                    const bool real = base.compare(0, 5, "float") == 0 || base.compare(0, 4, "half") == 0;
                    std::string one = "0";
                    if (component)
                    {
                        one = base.compare(0, 4, "bool") == 0
                            ? (component->IsBool() ? (component->boolean ? "true" : "false")
                                                   : (component->Uint() ? "true" : "false"))
                            : real ? Source::Float(component->Double())
                                   : Source::Int(component->Int());
                    }
                    items += (n ? ", " : "") + one;
                }
                const std::string cType = count > 1 ? base.substr(0, base.size() - 1) : base;
                w.Line(cType + " " + slot + "[" + std::to_string(count) + "] = {" + items + "};");
                if (c.Has("index"))
                {
                    w.Line("[" + var + " setConstantValue:" + slot + " type:" +
                        Source::Enum(MTL_TABLE(MTLDataType), (int64_t)type) + " atIndex:" +
                        Source::Uint(c.Uint("index")) + "];");
                }
                else
                {
                    w.Line("[" + var + " setConstantValue:" + slot + " type:" +
                        Source::Enum(MTL_TABLE(MTLDataType), (int64_t)type) + " withName:" +
                        Source::NSString(c.Str("name"), true) + "];");
                }
            }
            const std::string err = w.Local("error");
            w.Line("NSError* " + err + " = nil;");
            w.Line(name + " = [" + libraryName + " newFunctionWithName:" +
                Source::NSString(fname.UTF8String, true) + " constantValues:" + var + " error:&" + err + "];");
            w.Line("if (!" + name + ") Fail(\"function " + std::string(fname.UTF8String) + "\", " + err + ");");
        });
    }
    return function;
}

id MtlReplayer::CreateRenderPipeline(const JValue& object, const Decoder& d, uint64_t captureId)
{
    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    MTLRenderPipelineDescriptor* defaults = [[MTLRenderPipelineDescriptor alloc] init];
    FillVisitor fill(d);
    Reflect(fill, descriptor, defaults);
    descriptor.vertexFunction = (id<MTLFunction>)d.Object("vertexFunction");
    descriptor.fragmentFunction = (id<MTLFunction>)d.Object("fragmentFunction");

    // The vertex descriptor: layouts and attributes, each carrying the slot it sits in.
    const Decoder vertex = d.Nested("vertexDescriptor");
    MTLVertexDescriptor* vertexDescriptor = nil;
    // A pipeline with no vertex descriptor is recorded as one with no layouts and no attributes
    // (WriteVertexDescriptor in src/metal/src/hooks_descriptors.mm). Setting an empty one changes
    // nothing, so neither the replay nor the source it writes bothers.
    const bool hasVertexLayout = vertex.Json() &&
        ((vertex.Get("layouts") && vertex.Get("layouts")->count) ||
            (vertex.Get("attributes") && vertex.Get("attributes")->count));
    if (hasVertexLayout)
    {
        vertexDescriptor = [[MTLVertexDescriptor alloc] init];
        MTLVertexDescriptor* vertexDefaults = [[MTLVertexDescriptor alloc] init];
        if (const JValue* layouts = vertex.Get("layouts"))
        {
            for (uint32_t i = 0; i < layouts->count; ++i)
            {
                const Decoder l(&layouts->items[i], _env);
                const NSUInteger slot = (NSUInteger)l.Uint("index");
                FillVisitor f(l);
                Reflect(f, vertexDescriptor.layouts[slot], vertexDefaults.layouts[slot]);
            }
        }
        if (const JValue* attributes = vertex.Get("attributes"))
        {
            for (uint32_t i = 0; i < attributes->count; ++i)
            {
                const Decoder a(&attributes->items[i], _env);
                const NSUInteger slot = (NSUInteger)a.Uint("index");
                FillVisitor f(a);
                Reflect(f, vertexDescriptor.attributes[slot], vertexDefaults.attributes[slot]);
            }
        }
        descriptor.vertexDescriptor = vertexDescriptor;
    }

    const JValue* attachments = d.Get("colorAttachments");
    MTLRenderPipelineColorAttachmentDescriptor* blendDefaults =
        [[[MTLRenderPipelineDescriptor alloc] init] colorAttachments][0];
    if (attachments)
    {
        for (uint32_t i = 0; i < attachments->count; ++i)
        {
            const Decoder a(&attachments->items[i], _env);
            const NSUInteger slot = (NSUInteger)a.Uint("index");
            FillVisitor f(a);
            Reflect(f, descriptor.colorAttachments[slot], blendDefaults);
        }
    }

    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline = [_device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!pipeline)
    {
        Problem("render pipeline " + std::to_string(captureId) + " (" + LabelOf(captureId) + ") was not made: " +
            (error ? error.localizedDescription.UTF8String : "no error given"));
        return nil;
    }
    if (_x)
    {
        const std::string name = _x->Declare("id<MTLRenderPipelineState>", "pipeline", captureId, pipeline);
        _x->Block(MtlExporter::Create, LabelOf(captureId), [&](Source& w) {
            const std::string var = w.Local("descriptor");
            w.Line("MTLRenderPipelineDescriptor* " + var + " = [[MTLRenderPipelineDescriptor alloc] init];");
            EmitVisitor emit(w, var + ".");
            Reflect(emit, descriptor, defaults);
            if (descriptor.vertexFunction)
                w.Line(var + ".vertexFunction = " + w.Object(descriptor.vertexFunction) + ";");
            if (descriptor.fragmentFunction)
                w.Line(var + ".fragmentFunction = " + w.Object(descriptor.fragmentFunction) + ";");
            if (vertexDescriptor)
            {
                const std::string vd = w.Local("vertexDescriptor");
                w.Line("MTLVertexDescriptor* " + vd + " = [[MTLVertexDescriptor alloc] init];");
                MTLVertexDescriptor* vertexDefaults = [[MTLVertexDescriptor alloc] init];
                if (const JValue* layouts = vertex.Get("layouts"))
                {
                    for (uint32_t i = 0; i < layouts->count; ++i)
                    {
                        const NSUInteger slot = (NSUInteger)Decoder(&layouts->items[i], _env).Uint("index");
                        EmitVisitor e(w, vd + ".layouts[" + std::to_string(slot) + "].");
                        Reflect(e, vertexDescriptor.layouts[slot], vertexDefaults.layouts[slot]);
                    }
                }
                if (const JValue* attrs = vertex.Get("attributes"))
                {
                    for (uint32_t i = 0; i < attrs->count; ++i)
                    {
                        const NSUInteger slot = (NSUInteger)Decoder(&attrs->items[i], _env).Uint("index");
                        EmitVisitor e(w, vd + ".attributes[" + std::to_string(slot) + "].");
                        Reflect(e, vertexDescriptor.attributes[slot], vertexDefaults.attributes[slot]);
                    }
                }
                w.Line(var + ".vertexDescriptor = " + vd + ";");
            }
            if (attachments)
            {
                for (uint32_t i = 0; i < attachments->count; ++i)
                {
                    const NSUInteger slot = (NSUInteger)Decoder(&attachments->items[i], _env).Uint("index");
                    EmitVisitor e(w, var + ".colorAttachments[" + std::to_string(slot) + "].");
                    Reflect(e, descriptor.colorAttachments[slot], blendDefaults);
                }
            }
            const std::string err = w.Local("error");
            w.Line("NSError* " + err + " = nil;");
            w.Line(name + " = [device newRenderPipelineStateWithDescriptor:" + var + " error:&" + err + "];");
            w.Line("if (!" + name + ") Fail(\"render pipeline " + std::to_string(captureId) + "\", " + err + ");");
        });
    }
    return pipeline;
}

id MtlReplayer::CreateComputePipeline(const JValue& object, const Decoder& d, uint64_t captureId)
{
    id<MTLFunction> function = (id<MTLFunction>)d.Object("function");
    if (!function)
    {
        Problem("compute pipeline " + std::to_string(captureId) + " has no function in the replay");
        return nil;
    }
    MTLComputePipelineDescriptor* descriptor = [[MTLComputePipelineDescriptor alloc] init];
    MTLComputePipelineDescriptor* defaults = [[MTLComputePipelineDescriptor alloc] init];
    FillVisitor fill(d);
    Reflect(fill, descriptor, defaults);
    descriptor.computeFunction = function;
    // The intersection functions the kernel's traversal can call. Not part of the reflection macros
    // because MTLLinkedFunctions is an object rather than a property value, and the one thing that
    // makes a traced frame come out right rather than all-miss (mtl_raytracing.h).
    std::vector<std::pair<std::string, id>> linkedByName;
    if (id linked = LinkedFunctions(d, linkedByName))
    {
        if (@available(macOS 11.0, *))
            descriptor.linkedFunctions = (MTLLinkedFunctions*)linked;
    }

    NSError* error = nil;
    id<MTLComputePipelineState> pipeline =
        [_device newComputePipelineStateWithDescriptor:descriptor
                                               options:MTLPipelineOptionNone
                                            reflection:nil
                                                 error:&error];
    if (!pipeline)
    {
        Problem("compute pipeline " + std::to_string(captureId) + " was not made: " +
            (error ? error.localizedDescription.UTF8String : "no error given"));
        return nil;
    }
    if (_x)
    {
        const std::string name = _x->Declare("id<MTLComputePipelineState>", "compute_pipeline", captureId, pipeline);
        _x->Block(MtlExporter::Create, LabelOf(captureId), [&](Source& w) {
            const std::string var = w.Local("descriptor");
            w.Line("MTLComputePipelineDescriptor* " + var + " = [[MTLComputePipelineDescriptor alloc] init];");
            EmitVisitor emit(w, var + ".");
            Reflect(emit, descriptor, defaults);
            w.Line(var + ".computeFunction = " + w.Object(function) + ";");
            const std::string linked = WriteLinkedFunctionsSource(w, d);
            if (!linked.empty())
                w.Line(var + ".linkedFunctions = " + linked + ";");
            const std::string err = w.Local("error");
            w.Line("NSError* " + err + " = nil;");
            w.Line(name + " = [device newComputePipelineStateWithDescriptor:" + var +
                " options:MTLPipelineOptionNone reflection:nil error:&" + err + "];");
            w.Line("if (!" + name + ") Fail(\"compute pipeline " + std::to_string(captureId) + "\", " + err + ");");
        });
    }
    // Kept by name so an intersection function table made from this pipeline can be filled: an
    // entry names the function it runs, and a handle only exists for one the pipeline was linked
    // against.
    _linkedFunctions[captureId] = std::move(linkedByName);
    return pipeline;
}

id MtlReplayer::CreateDepthStencil(const JValue& object, const Decoder& d, uint64_t captureId)
{
    MTLDepthStencilDescriptor* descriptor = [[MTLDepthStencilDescriptor alloc] init];
    MTLDepthStencilDescriptor* defaults = [[MTLDepthStencilDescriptor alloc] init];
    FillVisitor fill(d);
    Reflect(fill, descriptor, defaults);
    const Decoder front = d.Nested("frontFaceStencil");
    const Decoder back = d.Nested("backFaceStencil");
    if (front.Json())
    {
        FillVisitor f(front);
        Reflect(f, descriptor.frontFaceStencil, defaults.frontFaceStencil);
    }
    if (back.Json())
    {
        FillVisitor f(back);
        Reflect(f, descriptor.backFaceStencil, defaults.backFaceStencil);
    }

    id<MTLDepthStencilState> state = [_device newDepthStencilStateWithDescriptor:descriptor];
    if (!state)
        return nil;
    if (_x)
    {
        const std::string name = _x->Declare("id<MTLDepthStencilState>", "depth_stencil", captureId, state);
        _x->Block(MtlExporter::Create, LabelOf(captureId), [&](Source& w) {
            const std::string var = w.Local("descriptor");
            w.Line("MTLDepthStencilDescriptor* " + var + " = [[MTLDepthStencilDescriptor alloc] init];");
            EmitVisitor emit(w, var + ".");
            Reflect(emit, descriptor, defaults);
            if (front.Json())
            {
                EmitVisitor e(w, var + ".frontFaceStencil.");
                Reflect(e, descriptor.frontFaceStencil, defaults.frontFaceStencil);
            }
            if (back.Json())
            {
                EmitVisitor e(w, var + ".backFaceStencil.");
                Reflect(e, descriptor.backFaceStencil, defaults.backFaceStencil);
            }
            w.Line(name + " = [device newDepthStencilStateWithDescriptor:" + var + "];");
        });
    }
    return state;
}

id MtlReplayer::CreateSampler(const JValue& object, const Decoder& d, uint64_t captureId)
{
    MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
    MTLSamplerDescriptor* defaults = [[MTLSamplerDescriptor alloc] init];
    FillVisitor fill(d);
    Reflect(fill, descriptor, defaults);
    id<MTLSamplerState> sampler = [_device newSamplerStateWithDescriptor:descriptor];
    if (!sampler)
        return nil;
    if (_x)
    {
        const std::string name = _x->Declare("id<MTLSamplerState>", "sampler", captureId, sampler);
        _x->Block(MtlExporter::Create, LabelOf(captureId), [&](Source& w) {
            const std::string var = w.Local("descriptor");
            w.Line("MTLSamplerDescriptor* " + var + " = [[MTLSamplerDescriptor alloc] init];");
            EmitVisitor emit(w, var + ".");
            Reflect(emit, descriptor, defaults);
            w.Line(name + " = [device newSamplerStateWithDescriptor:" + var + "];");
        });
    }
    return sampler;
}

id MtlReplayer::CreateHeap(const JValue& object, const Decoder& d, uint64_t captureId)
{
    MTLHeapDescriptor* descriptor = [[MTLHeapDescriptor alloc] init];
    MTLHeapDescriptor* defaults = [[MTLHeapDescriptor alloc] init];
    FillVisitor fill(d);
    Reflect(fill, descriptor, defaults);
    id<MTLHeap> heap = [_device newHeapWithDescriptor:descriptor];
    if (!heap)
        return nil;
    if (_x)
    {
        const std::string name = _x->Declare("id<MTLHeap>", "heap", captureId, heap);
        _x->Block(MtlExporter::Create, LabelOf(captureId), [&](Source& w) {
            const std::string var = w.Local("descriptor");
            w.Line("MTLHeapDescriptor* " + var + " = [[MTLHeapDescriptor alloc] init];");
            EmitVisitor emit(w, var + ".");
            Reflect(emit, descriptor, defaults);
            w.Line(name + " = [device newHeapWithDescriptor:" + var + "];");
        });
    }
    return heap;
}

/**
 * An acceleration structure. Made at the size the capture recorded, which is the size the
 * application's own structure came out at — a build writes into a structure the application had
 * already sized, so the size is what has to match, not the descriptor.
 *
 * Most applications use `newAccelerationStructureWithSize:` (test/path_tracer/metal does), in which
 * case the size is all there is; one created from a descriptor is made from the descriptor instead,
 * so the driver sizes it the way it sized the application's. A structure placed in a heap is made
 * from the device rather than from that heap: the replay is not reproducing the application's
 * memory layout, and a heap whose other placements differ would refuse the offset anyway.
 */
id MtlReplayer::CreateAccelerationStructure(const JValue& object, const Decoder& d, uint64_t captureId)
{
    if (@available(macOS 11.0, *))
    {
        const Decoder descriptor = d.Nested("descriptor");
        if (descriptor.Json())
        {
            std::string error;
            MTLAccelerationStructureDescriptor* rebuilt = BuildDescriptor(descriptor, error);
            if (rebuilt == nil)
            {
                Problem("acceleration structure " + std::to_string(captureId) + ": " + error);
                return nil;
            }
            id structure = [_device newAccelerationStructureWithDescriptor:rebuilt];
            if (structure == nil)
                return nil;
            if (_x)
            {
                const std::string name = _x->Declare("id<MTLAccelerationStructure>", "accel", captureId, structure);
                _x->Block(MtlExporter::Create, LabelOf(captureId), [&](Source& w) {
                    const std::string var = w.Local("descriptor");
                    w.Line("MTLAccelerationStructureDescriptor *" + var + " = nil;");
                    std::string why;
                    if (WriteDescriptorSource(w, var, descriptor, why))
                    {
                        w.Line(name + " = [device newAccelerationStructureWithDescriptor:" + var + "];");
                    }
                    else
                    {
                        w.Comment("left out: " + why);
                        w.Note("acceleration structure " + std::to_string(captureId) + ": " + why);
                    }
                });
            }
            return structure;
        }
        const uint64_t size = d.Uint("size");
        if (size == 0)
        {
            Problem("acceleration structure " + std::to_string(captureId) +
                " has neither a size nor a descriptor recorded");
            return nil;
        }
        id structure = [_device newAccelerationStructureWithSize:(NSUInteger)size];
        if (structure == nil)
            return nil;
        if (_x)
        {
            const std::string name = _x->Declare("id<MTLAccelerationStructure>", "accel", captureId, structure);
            _x->Block(MtlExporter::Create, LabelOf(captureId), [&](Source& w) {
                w.Line(name + " = [device newAccelerationStructureWithSize:" + std::to_string(size) + "];");
            });
        }
        return structure;
    }
    Problem("acceleration structures need macOS 11");
    return nil;
}

/**
 * An intersection or visible function table, made from the pipeline it belongs to and filled.
 *
 * This is where Metal's answer to a shader binding table lives, and it is the reason the Metal
 * replay needs none of the machinery the other two do. DXR writes a table into GPU memory as
 * 32-byte export identifiers, and Vulkan as opaque group handles, so both replays have to record
 * every identifier the capture's pipeline produced and substitute the replay's own into the
 * buffer's bytes (`Replayer::TraceRays`). A Metal table is set through the API, one entry at a
 * time, and an entry names its function — so the whole of that rewrite collapses into looking the
 * name up among the pipeline's linked functions and asking for a handle.
 *
 * The entries come from the object's `table` update rather than from commands: the capture sends
 * the whole table on every change, since an update is keyed and last-write-wins, so the update is
 * the table as it stood when the frame was captured (src/metal/src/raytracing.mm, SendTable).
 */
id MtlReplayer::CreateFunctionTable(const JValue& object, const Decoder& d, uint64_t captureId,
    bool intersection)
{
    if (@available(macOS 11.0, *))
    {
    }
    else
    {
        Problem("function tables need macOS 11");
        return nil;
    }
    const uint64_t pipelineId = d.ObjectId("pipeline");
    id pipeline = d.Object("pipeline");
    if (pipeline == nil)
    {
        Problem((intersection ? "intersection" : "visible") + std::string(" function table ") +
            std::to_string(captureId) + ": its pipeline is not in the replay");
        return nil;
    }
    if (![pipeline conformsToProtocol:@protocol(MTLComputePipelineState)])
    {
        Problem("function table " + std::to_string(captureId) +
            " belongs to a render pipeline, which the replay does not make tables from yet");
        return nil;
    }
    id<MTLComputePipelineState> compute = (id<MTLComputePipelineState>)pipeline;
    const Decoder table = Decoder(object.Get("updates"), _env).Nested("table");
    const uint64_t count = std::max<uint64_t>(d.Uint("functionCount"), table.Uint("functionCount"));

    id made = nil;
    std::string createdWith;
    if (intersection)
    {
        MTLIntersectionFunctionTableDescriptor* descriptor =
            [[MTLIntersectionFunctionTableDescriptor alloc] init];
        descriptor.functionCount = (NSUInteger)count;
        made = [compute newIntersectionFunctionTableWithDescriptor:descriptor];
        createdWith = "MTLIntersectionFunctionTableDescriptor";
    }
    else
    {
        MTLVisibleFunctionTableDescriptor* descriptor = [[MTLVisibleFunctionTableDescriptor alloc] init];
        descriptor.functionCount = (NSUInteger)count;
        made = [compute newVisibleFunctionTableWithDescriptor:descriptor];
        createdWith = "MTLVisibleFunctionTableDescriptor";
    }
    if (made == nil)
    {
        Problem("function table " + std::to_string(captureId) + " was not made by its pipeline");
        return nil;
    }

    std::string exportName;
    if (_x)
    {
        exportName = _x->Declare(intersection ? "id<MTLIntersectionFunctionTable>" : "id<MTLVisibleFunctionTable>",
            intersection ? "isect_table" : "visible_table", captureId, made);
    }
    _pendingTables.push_back({made, captureId, pipeline, pipelineId, intersection, count, createdWith, exportName});
    return made;
}

/**
 * Fills the tables made above, once every object exists.
 *
 * Deliberately a second pass rather than part of creation: objects are created in capture id order,
 * and a table is made from its pipeline before the buffers it binds for its functions are made —
 * the path tracer's table is object 7 and its buffers are 8 and 9. Filling at creation would report
 * both as missing and leave the intersection function with no arguments to read.
 */
void MtlReplayer::FillFunctionTables()
{
    for (const PendingTable& p : _pendingTables)
    {
        @autoreleasepool
        {
            FillFunctionTable(p);
        }
    }
    _pendingTables.clear();
}

void MtlReplayer::FillFunctionTable(const PendingTable& p)
{
    if (@available(macOS 11.0, *))
    {
    }
    else
    {
        return;
    }
    const uint64_t captureId = p.captureId;
    const bool intersection = p.intersection;
    id made = p.table;
    id pipeline = p.pipeline;
    id<MTLComputePipelineState> compute = (id<MTLComputePipelineState>)pipeline;
    const JValue* record = Record(captureId);
    const Decoder table = Decoder(record ? record->Get("updates") : nullptr, _env).Nested("table");

    const auto linked = _linkedFunctions.find(p.pipelineId);
    auto functionNamed = [&](const std::string& name) -> id {
        if (linked == _linkedFunctions.end())
            return nil;
        for (const auto& [n, f] : linked->second)
            if (n == name)
                return f;
        return nil;
    };
    std::vector<std::string> statements;

    const JValue* entries = table.Get("entries");
    for (uint32_t i = 0; entries && i < entries->count; ++i)
    {
        const Decoder entry = table.At(&entries->items[i]);
        const uint64_t slot = entry.Uint("index", i);
        // An entry the application never set: a ray reaching it calls nothing, which is what an
        // unset entry does here too, so it is left alone rather than reported.
        if (entry.Bool("empty"))
            continue;
        // An opaque triangle intersection function is Metal's own, not the application's: it is set
        // by signature rather than by a function, and the replay has no way to name it.
        if (entry.Has("opaque"))
        {
            Problem("function table " + std::to_string(captureId) + " entry " + std::to_string(slot) +
                " is an opaque triangle function, which the replay does not set");
            continue;
        }
        const std::string name = entry.Str("function");
        if (name.empty())
            continue;
        id function = functionNamed(name);
        if (function == nil)
        {
            Problem("function table " + std::to_string(captureId) + " entry " + std::to_string(slot) +
                " runs '" + name + "', which is not among the pipeline's linked functions in the replay");
            continue;
        }
        id<MTLFunctionHandle> handle = [compute functionHandleWithFunction:(id<MTLFunction>)function];
        if (handle == nil)
        {
            Problem("function table " + std::to_string(captureId) + " entry " + std::to_string(slot) +
                ": the pipeline gives no handle for '" + name + "'");
            continue;
        }
        if (intersection)
        {
            [(id<MTLIntersectionFunctionTable>)made setFunction:handle atIndex:(NSUInteger)slot];
        }
        else
        {
            [(id<MTLVisibleFunctionTable>)made setFunction:handle atIndex:(NSUInteger)slot];
        }
        if (_x)
        {
            statements.push_back("[" + p.exportName + " setFunction:[" + ExportName(pipeline) +
                " functionHandleWithFunction:" + ExportName(function) + "] atIndex:" +
                std::to_string(slot) + "];");
        }
    }

    // The buffers the table binds for its functions, which an intersection function reads as its
    // own arguments. Intersection tables only: a visible function table has no buffers.
    const JValue* buffers = intersection ? table.Get("buffers") : nullptr;
    for (uint32_t i = 0; buffers && i < buffers->count; ++i)
    {
        const Decoder entry = table.At(&buffers->items[i]);
        const uint64_t slot = entry.Uint("index");
        const uint64_t offset = entry.Uint("offset");
        id<MTLBuffer> buffer = (id<MTLBuffer>)Object(entry.Uint("buffer"));
        if (buffer == nil)
        {
            Problem("function table " + std::to_string(captureId) + " binds buffer " +
                std::to_string(entry.Uint("buffer")) + " at " + std::to_string(slot) +
                ", which is not in the replay");
            continue;
        }
        [(id<MTLIntersectionFunctionTable>)made setBuffer:buffer offset:(NSUInteger)offset atIndex:(NSUInteger)slot];
        if (_x)
        {
            statements.push_back("[" + p.exportName + " setBuffer:" + ExportName(buffer) + " offset:" +
                std::to_string(offset) + " atIndex:" + std::to_string(slot) + "];");
        }
    }

    if (_x)
    {
        _x->Block(MtlExporter::Create, LabelOf(captureId), [&](Source& w) {
            const std::string var = w.Local("descriptor");
            w.Line(p.createdWith + " *" + var + " = [[" + p.createdWith + " alloc] init];");
            w.Line(var + ".functionCount = " + std::to_string(p.functionCount) + ";");
            w.Line(p.exportName + " = [" + w.Object(pipeline) +
                (intersection ? " newIntersectionFunctionTableWithDescriptor:" : " newVisibleFunctionTableWithDescriptor:") +
                var + "];");
            for (const std::string& statement : statements)
                w.Line(statement);
        });
    }
}

// ------------------------------------------------------------------------------------------
// Contents

void MtlReplayer::WriteBuffer(id<MTLBuffer> buffer, uint64_t offset, const void* data, uint64_t size,
    const std::string& what)
{
    if (!buffer || !size)
        return;
    if (offset + size > buffer.length)
    {
        Problem(what + ": " + std::to_string(size) + " bytes at " + std::to_string(offset) +
            " do not fit a buffer of " + std::to_string(buffer.length));
        return;
    }
    if (buffer.storageMode != MTLStorageModePrivate)
    {
        std::memcpy((uint8_t*)buffer.contents + offset, data, (size_t)size);
        if (buffer.storageMode == MTLStorageModeManaged)
        {
            [buffer didModifyRange:NSMakeRange((NSUInteger)offset, (NSUInteger)size)];
        }
        return;
    }
    id<MTLBuffer> staging = [_device newBufferWithBytes:data
                                                 length:(NSUInteger)size
                                                options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> commands = [_queue commandBuffer];
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

void MtlReplayer::UploadTexture(id<MTLTexture> texture, const JValue& info, const uint8_t* data, size_t size)
{
    const mtlinsp::PixelFormatInfo format = mtlinsp::PixelFormatDetails(texture.pixelFormat);
    if (!format.blockBytes)
    {
        Problem("texture " + std::to_string(info.Get("id") ? info.Get("id")->Uint() : 0) +
            " has a pixel format with no known layout, so its contents were not uploaded");
        return;
    }
    const uint32_t mip = info.Get("mip") ? (uint32_t)info.Get("mip")->Uint() : 0;
    const uint32_t width = (uint32_t)std::max<NSUInteger>(1, texture.width >> mip);
    const uint32_t height = (uint32_t)std::max<NSUInteger>(1, texture.height >> mip);
    uint64_t rowBytes = 0;
    const uint64_t imageBytes = mtlinsp::PixelFormatImageSize(format, width, height, &rowBytes);
    if (!imageBytes || imageBytes > size)
    {
        Problem("texture contents are " + std::to_string(size) + " bytes, and one mip needs " +
            std::to_string(imageBytes));
        return;
    }
    const uint32_t slice = info.Get("baseLayer") ? (uint32_t)info.Get("baseLayer")->Uint() : 0;
    // Through a staging buffer, because a private texture — which every render target is — cannot
    // be written with replaceRegion:.
    id<MTLBuffer> staging = [_device newBufferWithBytes:data
                                                 length:(NSUInteger)imageBytes
                                                options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> commands = [_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromBuffer:staging
               sourceOffset:0
          sourceBytesPerRow:(NSUInteger)rowBytes
        sourceBytesPerImage:(NSUInteger)imageBytes
                 sourceSize:MTLSizeMake(width, height, 1)
                  toTexture:texture
           destinationSlice:slice
           destinationLevel:mip
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
}

void MtlReplayer::UploadContents()
{
    // The textures a draw sampled, and whatever an image held when the frame first read it. A
    // render target the frame writes before it reads needs nothing: the frame fills it.
    if (_x && !_textureData.empty())
    {
        _x->Comment(MtlExporter::Contents, "The contents the frame starts from: what it sampled, read back with the capture.");
    }
    for (const auto& [captureId, entry] : _textureData)
    {
        @autoreleasepool
        {
            const JValue* info = entry->Get("info");
            if (!info || !info->Get("id"))
                continue;
            const uint64_t textureId = info->Get("id")->Uint();
            if (!_uploadedTextures.insert(captureId).second)
                continue;
            id<MTLTexture> texture = (id<MTLTexture>)Object(textureId);
            if (!texture)
                continue;
            if (info->Get("error"))
            {
                Problem("texture " + std::to_string(textureId) + " was not read back by the capture: " +
                    Text(info->Get("error")));
                continue;
            }
            const uint8_t* data = nullptr;
            size_t size = 0;
            if (!_capture.Payload(entry->Get("payload"), data, size) || !size)
                continue;
            UploadTexture(texture, *info, data, size);
            if (_x)
            {
                const mtlinsp::PixelFormatInfo format = mtlinsp::PixelFormatDetails(texture.pixelFormat);
                const uint32_t mip = info->Get("mip") ? (uint32_t)info->Get("mip")->Uint() : 0;
                const uint32_t width = (uint32_t)std::max<NSUInteger>(1, texture.width >> mip);
                const uint32_t height = (uint32_t)std::max<NSUInteger>(1, texture.height >> mip);
                uint64_t rowBytes = 0;
                const uint64_t imageBytes = mtlinsp::PixelFormatImageSize(format, width, height, &rowBytes);
                const uint32_t slice = info->Get("baseLayer") ? (uint32_t)info->Get("baseLayer")->Uint() : 0;
                const std::string name = _x->NameOf(texture);
                const std::string where = _x->Data(data, (size_t)std::min<uint64_t>(imageBytes, size));
                _x->Block(MtlExporter::Contents, "texture " + std::to_string(textureId), [&](Source& w) {
                    w.Line("UploadTexture(" + name + ", " + where + ", " + Source::Uint(imageBytes) + ", " +
                        Source::Uint(rowBytes) + ", " + Source::Uint(width) + ", " + Source::Uint(height) +
                        ", " + Source::Uint(slice) + ", " + Source::Uint(mip) + ");");
                });
            }
        }
    }
}

// ------------------------------------------------------------------------------------------
// The frame

void MtlReplayer::BeginCommandBuffer(uint64_t captureId)
{
    _commandBufferId = captureId;
    _commandBuffer = [_queue commandBuffer];
    _passCounter = 0;
    if (_x)
    {
        // A global for the same reason the encoders are: the parts `Frame` is cut into are
        // functions, and a command buffer outlives any one of them. One name serves every command
        // buffer, since the replay commits and waits for each before opening the next.
        if (_commandBufferVar.empty())
        {
            _commandBufferVar = "commands";
            _x->Global("id<MTLCommandBuffer>", _commandBufferVar);
        }
        _x->Blank(MtlExporter::Frame);
        _x->Comment(MtlExporter::Frame, "command buffer " + std::to_string(captureId) + (LabelOf(captureId).empty() ? "" : " \"" + LabelOf(captureId) + "\""));
        _x->Block(MtlExporter::Frame, "", [&](Source& w) {
            w.Line(_commandBufferVar + " = [queue commandBuffer];");
        });
    }
}

/** Whether this selector opens an encoder; every other command runs against the one already open. */
bool MtlReplayer::OpensEncoder(const std::string& m)
{
    return m == "renderCommandEncoderWithDescriptor:" || m == "parallelRenderCommandEncoderWithDescriptor:" ||
        m == "renderCommandEncoder" || m.compare(0, 21, "computeCommandEncoder") == 0 ||
        m.compare(0, 18, "blitCommandEncoder") == 0 ||
        m.compare(0, 27, "resourceStateCommandEncoder") == 0 ||
        m.compare(0, 35, "accelerationStructureCommandEncoder") == 0;
}

void MtlReplayer::Commit(bool last)
{
    if (!_commandBuffer)
        return;
    // A sub-encoder leaves its parallel parent open behind it, so this closes both.
    while (_encoder)
        EndEncoder();
    [_commandBuffer commit];
    _committed.push_back(_commandBuffer);
    ++_submissionCount;
    if (_x)
    {
        _x->CountSubmission();
        _x->Block(MtlExporter::Frame, "", [&](Source& w) { w.Line("[" + _commandBufferVar + " commit];"); });
        _x->Block(MtlExporter::Frame, "", [&](Source& w) { w.Line("[" + _commandBufferVar + " waitUntilCompleted];"); });
        _x->EndSubmission();
    }
    _commandBuffer = nil;
    _commandBufferId = 0;
}

void MtlReplayer::EndEncoder()
{
    if (!_encoder)
        return;
    // A parallel encoder's sub-encoder shares the pass, so the read-backs wait for the parent.
    const bool sub = _pass.parentId != 0;
    // Every binding of this encoder is now known, so the verdict on what it wrote can be taken: a
    // kernel that also read a texture it writes makes everything it wrote unreproducible
    // (CompareWrittenTextures). A texture written by two encoders keeps the worse verdict.
    for (uint64_t textureId : _encoderWrites)
    {
        auto [it, inserted] = _writtenTextures.insert({textureId, !_encoderAccumulates});
        if (!inserted && _encoderAccumulates)
            it->second = false;
    }
    _encoderWrites.clear();
    _encoderAccumulates = false;
    [(id<MTLCommandEncoder>)_encoder endEncoding];
    if (_x)
    {
        _x->Block(MtlExporter::Frame, "", [&](Source& w) { w.Line("[" + _pass.variable + " endEncoding];"); });
    }
    if (!sub)
        QueuePassReadbacks();
    _encoder = nil;
    if (sub)
    {
        // Back to the parallel encoder it came from: the next sub-encoder, or its own end.
        _encoder = _parallel;
        _pass = _parallelPass;
        _parallel = nil;
    }
    else
    {
        _pass = OpenPass();
        _parallel = nil;
    }
}

void MtlReplayer::ReplayCommands()
{
    const JValue* commands = _capture.Commands();
    if (!commands)
        return;
    for (uint32_t i = 0; i < commands->count; ++i)
    {
        @autoreleasepool
        {
            const JValue& command = commands->items[i];
            const std::string method = Text(command.Get("method"));
            const uint32_t index = command.Get("index") ? (uint32_t)command.Get("index")->Uint() : i;
            const uint64_t owner = IdOf(command.Get("object"));
            if (_options.trace)
                std::fprintf(stderr, "[%u] %s\n", index, method.c_str());

            if (owner && owner != _commandBufferId)
            {
                if (_commandBuffer)
                    Commit(false);
                BeginCommandBuffer(owner);
            }
            _env.where = "command " + std::to_string(index) + " (" + method + ")";
            const Decoder d(command.Get("args"), _env);
            IssueCommand(index, method, d, command);
            _env.where.clear();
        }
    }
    if (_commandBuffer)
        Commit(true);
    for (id<MTLCommandBuffer> committed : _committed)
        [committed waitUntilCompleted];
    for (id<MTLCommandBuffer> committed : _committed)
    {
        if (committed.error)
        {
            Problem(std::string("a command buffer failed: ") + committed.error.localizedDescription.UTF8String);
        }
    }
}

void MtlReplayer::IssueCommand(uint32_t index, const std::string& method, const Decoder& d,
    const JValue& command)
{
    const uint64_t encoderId = IdOf(command.Get("encoder"));
    BindBufferContents(d, command);

    if (OpensEncoder(method))
    {
        OpenEncoder(method, d, index, encoderId);
        ++_commandCount;
        return;
    }
    // A command on an encoder that is not the one open: the stream has moved off a parallel
    // encoder's sub-encoder back to the parent, which is the only way that happens. The
    // sub-encoder has no recorded endEncoding of its own — its end is not the pass's — so this is
    // where it is closed.
    if (encoderId && encoderId != _pass.encoderId && _parallel && encoderId == _parallelPass.encoderId)
    {
        EndEncoder();
    }
    if (CommandBufferCommand(method, d, index))
    {
        ++_commandCount;
        return;
    }
    if (!_encoder)
    {
        LeftOut(index, method, "no encoder is open");
        return;
    }
    if (CommonCommand(method, d, index))
    {
        ++_commandCount;
        return;
    }
    bool handled = false;
    if ([_encoder conformsToProtocol:@protocol(MTLRenderCommandEncoder)])
        handled = RenderCommand(method, d, index);
    else if ([_encoder conformsToProtocol:@protocol(MTLComputeCommandEncoder)])
        handled = ComputeCommand(method, d, index);
    else if ([_encoder conformsToProtocol:@protocol(MTLBlitCommandEncoder)])
        handled = BlitCommand(method, d, index);
    else if ([_encoder conformsToProtocol:@protocol(MTLAccelerationStructureCommandEncoder)])
        handled = AccelerationCommand(method, d, index);
    if (handled)
        ++_commandCount;
    else
        LeftOut(index, method, "the replay has no handler for it");
}

/** The contents of every buffer range a command binds, written before the command buffer runs. */
void MtlReplayer::BindBufferContents(const Decoder& d, const JValue& command)
{
    // An acceleration structure build names its geometry's contents in `buildData` rather than in
    // `bufferData`, and as objects rather than as bare ids: an input is keyed by geometry and field
    // so the UI can say *which* buffer of the build a range belongs to (src/metal/src/raytracing.mm,
    // InputsJson). Uploading them matters more here than for any ordinary bind — a build whose
    // vertices were never written reads zeros, builds a structure with nothing in it, and every ray
    // of the frame misses. Which is a failure that looks exactly like a working replay.
    // In the command's *arguments*, unlike `bufferData` below, which the capture writes beside them:
    // a build's inputs are part of what the command says (src/metal/src/raytracing.mm).
    if (const JValue* inputs = d.Get("buildData"))
    {
        if (inputs->IsArray())
        {
            for (uint32_t i = 0; i < inputs->count; ++i)
            {
                const JValue* capture = inputs->items[i].Get("capture");
                if (!capture)
                    continue;
                UploadBufferRange(capture->Uint());
            }
        }
    }
    const JValue* ids = command.Get("bufferData");
    if (!ids || !ids->IsArray())
        return;
    for (uint32_t i = 0; i < ids->count; ++i)
        UploadBufferRange(ids->items[i].Uint());
}

/** One CaptureBuffers range, written into the buffer it belongs to; each is written once. */
void MtlReplayer::UploadBufferRange(uint64_t dataId)
{
    if (!dataId || !_uploadedBuffers.insert(dataId).second)
        return;
    const auto it = _bufferData.find(dataId);
    if (it == _bufferData.end())
        return;
    const JValue* info = it->second->Get("info");
    if (!info)
        return;
    const uint64_t bufferId = info->Get("buffer") ? info->Get("buffer")->Uint() : 0;
    if (!bufferId)
        return;   // inline bytes, which the command carries itself
    id<MTLBuffer> buffer = (id<MTLBuffer>)Object(bufferId);
    if (!buffer)
        return;
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!_capture.Payload(it->second->Get("payload"), data, size) || !size)
        return;
    const uint64_t offset = info->Get("offset") ? info->Get("offset")->Uint() : 0;
    if (_options.trace)
        std::fprintf(stderr, "upload buffer %llu +%llu %zu bytes (data %llu)\n",
            (unsigned long long)bufferId, (unsigned long long)offset, size,
            (unsigned long long)dataId);
    WriteBuffer(buffer, offset, data, size, "buffer " + std::to_string(bufferId));
    if (_x)
    {
        const std::string name = _x->NameOf(buffer);
        const std::string where = _x->Data(data, size);
        _x->Block(MtlExporter::Contents, "buffer " + std::to_string(bufferId), [&](Source& w) {
            w.Line("UploadBuffer(" + name + ", " + Source::Uint(offset) + ", " + where + ", " +
                Source::Uint(size) + ");");
        });
    }
}

} // namespace mtlreplay
