#include "dx_support.h"

#include "frame_window.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

ID3D12Device* device = nullptr;

namespace
{

struct Readback
{
    ID3D12Resource* buffer = nullptr;
    std::string name;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    int aspect = 0;
    UINT width = 0;
    UINT height = 0;
    UINT64 rowBytes = 0;
    UINT rows = 0;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    const uint8_t* captured = nullptr;
    UINT64 capturedSize = 0;
    std::string note;
    // After the comparison:
    bool compared = false;
    uint64_t texels = 0;
    uint64_t differing = 0;
    uint32_t maxByteDelta = 0;
    std::vector<uint8_t> replayed;
};

IDXGIFactory4* factory = nullptr;
ID3D12InfoQueue* infoQueue = nullptr;
ID3D12CommandQueue* supportQueue = nullptr;
ID3D12CommandAllocator* supportAllocator = nullptr;
ID3D12GraphicsCommandList* supportList = nullptr;
ID3D12Fence* fence = nullptr;
HANDLE fenceEvent = nullptr;
UINT64 fenceValue = 0;
std::string adapterText;
std::vector<uint8_t> dataBytes;
std::vector<ID3D12Object*> owned;          // closed lists' allocators and the lists the support made
std::vector<ID3D12Resource*> transients;   // staging and resolve resources of the submission being built
std::vector<Readback> pending;
std::vector<Readback> results;
size_t debugErrors = 0;
std::set<std::string> debugSeen;           // a frame that runs in a loop says each thing once

// Buffer uploads are gathered into one list and one wait: a frame binds thousands of ranges, and a
// list, a staging buffer and a wait for each made a frame that runs in a loop run at a few frames a
// second. The staging memory is kept from frame to frame and stays mapped.
struct UploadChunk
{
    ID3D12Resource* buffer = nullptr;
    uint8_t* mapped = nullptr;
    UINT64 size = 0;
    UINT64 used = 0;
};
std::vector<UploadChunk> uploadChunks;
ID3D12CommandAllocator* uploadAllocator = nullptr;
ID3D12GraphicsCommandList* uploadList = nullptr;
bool uploadsOpen = false;

// The window
FrameWindow* window = nullptr;
IDXGISwapChain3* swapChain = nullptr;
std::string windowTitle;
bool vsync = true;
uint64_t framesShown = 0;
uint64_t framesAtTitle = 0;
std::chrono::steady_clock::time_point titleTime;

void Wait(ID3D12CommandQueue* queue)
{
    const UINT64 value = ++fenceValue;
    DX_CHECK(queue->Signal(fence, value));
    if (fence->GetCompletedValue() < value)
    {
        fence->SetEventOnCompletion(value, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
}

ID3D12Resource* CreateBuffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<UINT64>(size, 1);
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buffer = nullptr;
    DX_CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer)));
    return buffer;
}

void PrintDebugMessages()
{
    if (!infoQueue)
        return;
    const UINT64 count = infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T size = 0;
        infoQueue->GetMessage(i, nullptr, &size);
        std::vector<char> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        if (FAILED(infoQueue->GetMessage(i, message, &size)) || message->Severity > D3D12_MESSAGE_SEVERITY_WARNING)
            continue;
        const bool error = message->Severity < D3D12_MESSAGE_SEVERITY_WARNING;
        if (!debugSeen.insert(message->pDescription).second)
            continue;
        if (error)
            ++debugErrors;
        std::fprintf(stderr, "debug layer %s: %s\n", error ? "error" : "warning", message->pDescription);
    }
    infoQueue->ClearStoredMessages();
}

// ---- PNG output (stored deflate blocks: large files, no zlib)

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready)
    {
        for (uint32_t n = 0; n < 256; ++n)
        {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
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
        const uint32_t len = (uint32_t)data.size();
        const uint8_t be[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
        out.write((const char*)be, 4);
        out.write((const char*)body.data(), (std::streamsize)body.size());
        const uint32_t crc = Crc32(body.data(), body.size(), 0);
        const uint8_t cb[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
        out.write((const char*)cb, 4);
    };
    const std::vector<uint8_t> ihdr = {(uint8_t)(width >> 24), (uint8_t)(width >> 16), (uint8_t)(width >> 8), (uint8_t)width,
        (uint8_t)(height >> 24), (uint8_t)(height >> 16), (uint8_t)(height >> 8), (uint8_t)height,
        8, 6, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return true;
}

/** 8-bit RGBA and BGRA as they are, 32-bit float depth stretched to its range; false for other formats. */
bool ToRgba(const Readback& r, const uint8_t* bytes, size_t size, std::vector<uint8_t>& rgba)
{
    const size_t texels = (size_t)r.width * r.height;
    rgba.assign(texels * 4, 255);
    const bool bgr = r.format == DXGI_FORMAT_B8G8R8A8_UNORM || r.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool rgb = r.format == DXGI_FORMAT_R8G8B8A8_UNORM || r.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if ((bgr || rgb) && r.aspect == 0)
    {
        if (size < texels * 4)
            return false;
        for (size_t i = 0; i < texels; ++i)
        {
            rgba[i * 4] = bytes[i * 4 + (bgr ? 2 : 0)];
            rgba[i * 4 + 1] = bytes[i * 4 + 1];
            rgba[i * 4 + 2] = bytes[i * 4 + (bgr ? 0 : 2)];
        }
        return true;
    }
    const bool d32 = r.format == DXGI_FORMAT_D32_FLOAT || r.format == DXGI_FORMAT_R32_TYPELESS || r.format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
        r.format == DXGI_FORMAT_R32G8X24_TYPELESS;
    if (d32 && r.aspect == 1)
    {
        if (size < texels * 4)
            return false;
        float lo = INFINITY, hi = -INFINITY;
        for (size_t i = 0; i < texels; ++i)
        {
            float v;
            std::memcpy(&v, &bytes[i * 4], 4);
            if (std::isfinite(v))
            {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        for (size_t i = 0; i < texels; ++i)
        {
            float v;
            std::memcpy(&v, &bytes[i * 4], 4);
            const uint8_t g = hi > lo ? (uint8_t)std::lround(255.0 * (v - lo) / (hi - lo)) : 0;
            rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = g;
        }
        return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------------------------

void Fail(const char* what, HRESULT result)
{
    PrintDebugMessages();
    std::fprintf(stderr, "%s returned 0x%08lx\n", what, (unsigned long)result);
    if (device && result == DXGI_ERROR_DEVICE_REMOVED)
        std::fprintf(stderr, "the device was removed: 0x%08lx\n", (unsigned long)device->GetDeviceRemovedReason());
    std::fflush(stderr);
    std::exit(2);
}

void Fail(const std::string& message)
{
    PrintDebugMessages();
    std::fprintf(stderr, "%s\n", message.c_str());
    std::fflush(stderr);
    std::exit(2);
}

bool LoadData(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    dataBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

const void* Data(uint64_t offset, uint64_t size)
{
    if (offset + size > dataBytes.size())
        Fail("the data file is shorter than the frame expects (offset " + std::to_string(offset) + ", " + std::to_string(size) + " bytes)");
    return dataBytes.data() + offset;
}

void CreateDeviceOn(const char* adapterName, D3D_FEATURE_LEVEL level, bool debugLayer)
{
    if (debugLayer)
    {
        ID3D12Debug* debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            debug->Release();
        }
        else
        {
            std::fprintf(stderr, "the D3D12 debug layer is not installed (Windows' Graphics Tools); running without it\n");
        }
    }
    DX_CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    IDXGIAdapter1* chosen = nullptr;
    IDXGIAdapter1* first = nullptr;
    for (UINT i = 0; !chosen; ++i)
    {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        std::string name;
        for (const wchar_t* c = desc.Description; *c; ++c)
            name += *c < 0x80 ? (char)*c : '?';
        if (adapterName && name == adapterName)
            chosen = adapter;
        else if (!first && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            first = adapter;
        else
            adapter->Release();
    }
    if (!chosen)
        chosen = first;
    else if (first)
        first->Release();
    if (chosen)
    {
        DXGI_ADAPTER_DESC1 desc{};
        chosen->GetDesc1(&desc);
        for (const wchar_t* c = desc.Description; *c; ++c)
            adapterText += *c < 0x80 ? (char)*c : '?';
    }
    if (adapterName && *adapterName && adapterText != adapterName)
        std::fprintf(stderr, "the frame was captured on %s; running on %s\n", adapterName, adapterText.c_str());
    DX_CHECK(D3D12CreateDevice(chosen, level, IID_PPV_ARGS(&device)));
    if (chosen)
        chosen->Release();
    if (debugLayer)
        device->QueryInterface(IID_PPV_ARGS(&infoQueue));

    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    DX_CHECK(device->CreateCommandQueue(&queue, IID_PPV_ARGS(&supportQueue)));
    DX_CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&supportAllocator)));
    DX_CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, supportAllocator, nullptr, IID_PPV_ARGS(&supportList)));
    DX_CHECK(supportList->Close());
    DX_CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

const char* AdapterName() { return adapterText.c_str(); }

ID3D12RootSignature* CreateRootSignatureFrom(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc)
{
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3D12SerializeVersionedRootSignature(desc, &blob, &errors);
    if (FAILED(hr))
    {
        if (errors)
            std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        Fail("D3D12SerializeVersionedRootSignature", hr);
    }
    if (errors)
        errors->Release();
    ID3D12RootSignature* root = nullptr;
    DX_CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)));
    blob->Release();
    return root;
}

ID3D12GraphicsCommandList* CreateClosedCommandList(D3D12_COMMAND_LIST_TYPE type)
{
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    DX_CHECK(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator)));
    DX_CHECK(device->CreateCommandList(0, type, allocator, nullptr, IID_PPV_ARGS(&list)));
    DX_CHECK(list->Close());
    owned.push_back(allocator);
    return list;
}

D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(ID3D12DescriptorHeap* heap, UINT index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)index * device->GetDescriptorHandleIncrementSize(heap->GetDesc().Type);
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(ID3D12DescriptorHeap* heap, UINT index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += (UINT64)index * device->GetDescriptorHandleIncrementSize(heap->GetDesc().Type);
    return handle;
}

/** Executes the buffer uploads gathered so far, before anything that reads what they write. */
void FlushUploads()
{
    if (!uploadsOpen)
        return;
    uploadsOpen = false;
    DX_CHECK(uploadList->Close());
    ID3D12CommandList* lists[] = {uploadList};
    supportQueue->ExecuteCommandLists(1, lists);
    Wait(supportQueue);
    for (UploadChunk& c : uploadChunks)
        c.used = 0;
}

/** `size` bytes of mapped upload memory, with the buffer and the offset to copy them from. */
uint8_t* UploadSpace(UINT64 size, ID3D12Resource** buffer, UINT64* offset)
{
    const UINT64 aligned = (size + 255) & ~255ull;
    for (UploadChunk& c : uploadChunks)
    {
        if (c.used + aligned > c.size)
            continue;
        *buffer = c.buffer;
        *offset = c.used;
        c.used += aligned;
        return c.mapped + *offset;
    }
    UploadChunk c;
    c.size = std::max<UINT64>(aligned, 16ull << 20);
    c.buffer = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, c.size, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    DX_CHECK(c.buffer->Map(0, &none, &mapped));
    c.mapped = static_cast<uint8_t*>(mapped);
    c.used = aligned;
    uploadChunks.push_back(c);
    *buffer = c.buffer;
    *offset = 0;
    return c.mapped;
}

ID3D12GraphicsCommandList* BeginOneTime()
{
    FlushUploads();
    DX_CHECK(supportAllocator->Reset());
    DX_CHECK(supportList->Reset(supportAllocator, nullptr));
    return supportList;
}

void EndOneTime(ID3D12GraphicsCommandList* list)
{
    DX_CHECK(list->Close());
    ID3D12CommandList* lists[] = {list};
    supportQueue->ExecuteCommandLists(1, lists);
    Wait(supportQueue);
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, UINT subresource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = subresource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

void UploadTexture(ID3D12Resource* texture, const TextureRegion* regions, UINT count, const void* data, UINT64 size)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
    UINT64 stagingSize = 0;
    for (UINT k = 0; k < count; ++k)
    {
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, regions[k].subresource, 1, stagingSize, &footprints[k], nullptr, nullptr, &bytes);
        stagingSize = (footprints[k].Offset + bytes + 511) & ~511ull;
    }
    ID3D12Resource* staging = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, stagingSize, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    DX_CHECK(staging->Map(0, nullptr, &mapped));
    // Tight rows in the data; the copy wants them at the footprint's pitch.
    const uint8_t* at = static_cast<const uint8_t*>(data);
    const uint8_t* end = at + size;
    for (UINT k = 0; k < count; ++k)
    {
        uint8_t* base = static_cast<uint8_t*>(mapped) + footprints[k].Offset;
        const UINT64 slicePitch = (UINT64)footprints[k].Footprint.RowPitch * regions[k].rows;
        for (UINT z = 0; z < regions[k].slices; ++z)
            for (UINT row = 0; row < regions[k].rows && at + regions[k].rowBytes <= end; ++row, at += regions[k].rowBytes)
                std::memcpy(base + z * slicePitch + (UINT64)row * footprints[k].Footprint.RowPitch, at, (size_t)regions[k].rowBytes);
    }
    staging->Unmap(0, nullptr);
    ID3D12GraphicsCommandList* list = BeginOneTime();
    for (UINT k = 0; k < count; ++k)
    {
        Transition(list, texture, regions[k].subresource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = regions[k].subresource;
        D3D12_TEXTURE_COPY_LOCATION src{staging, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = footprints[k];
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    EndOneTime(list);
    staging->Release();
}

void UploadBuffer(ID3D12Resource* buffer, UINT64 offset, const void* data, UINT64 size, D3D12_RESOURCE_STATES state, bool uploadHeap)
{
    if (uploadHeap)
    {
        void* mapped = nullptr;
        const D3D12_RANGE none{0, 0};
        DX_CHECK(buffer->Map(0, &none, &mapped));
        std::memcpy(static_cast<uint8_t*>(mapped) + offset, data, (size_t)size);
        buffer->Unmap(0, nullptr);
        return;
    }
    // Into the list of gathered uploads (FlushUploads), which runs before the submission they are for.
    if (!uploadList)
    {
        DX_CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&uploadAllocator)));
        DX_CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAllocator, nullptr, IID_PPV_ARGS(&uploadList)));
        uploadsOpen = true;
    }
    else if (!uploadsOpen)
    {
        DX_CHECK(uploadAllocator->Reset());
        DX_CHECK(uploadList->Reset(uploadAllocator, nullptr));
        uploadsOpen = true;
    }
    ID3D12Resource* staging = nullptr;
    UINT64 stagingOffset = 0;
    std::memcpy(UploadSpace(size, &staging, &stagingOffset), data, (size_t)size);
    Transition(uploadList, buffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, D3D12_RESOURCE_STATE_COPY_DEST);
    uploadList->CopyBufferRegion(buffer, offset, staging, stagingOffset, size);
    Transition(uploadList, buffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, state);
}

void ReadbackTexture(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, const char* name, UINT firstSubresource, UINT count,
    D3D12_RESOURCE_STATES state, DXGI_FORMAT resolveFormat, DXGI_FORMAT format, int aspect, UINT width, UINT height,
    UINT64 rowBytes, UINT rows, const void* captured, UINT64 capturedSize)
{
    if (window)
        return;   // shown, not compared: the comparison is --batch's
    Readback r;
    r.name = name;
    r.format = format;
    r.aspect = aspect;
    r.width = width;
    r.height = height;
    r.rowBytes = rowBytes;
    r.rows = rows;
    r.captured = static_cast<const uint8_t*>(captured);
    r.capturedSize = capturedSize;
    D3D12_RESOURCE_DESC desc = texture->GetDesc();
    ID3D12Resource* source = texture;
    if (resolveFormat != DXGI_FORMAT_UNKNOWN)
    {
        // A multisampled target, read as the capture read it: through a resolve into a texture of the support's own.
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC resolved = desc;
        resolved.SampleDesc = {1, 0};
        resolved.Alignment = 0;   // a multisampled resource's alignment is not valid for one that is not
        resolved.Flags = D3D12_RESOURCE_FLAG_NONE;
        resolved.MipLevels = 1;
        resolved.DepthOrArraySize = 1;
        resolved.Width = width;
        resolved.Height = height;
        DX_CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resolved, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&source)));
        transients.push_back(source);
        Transition(list, texture, firstSubresource, state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        list->ResolveSubresource(source, 0, texture, firstSubresource, resolveFormat);
        Transition(list, texture, firstSubresource, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, state);
        Transition(list, source, 0, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        desc = resolved;
        firstSubresource = 0;
        count = 1;
    }
    UINT64 total = 0;
    for (UINT k = 0; k < count; ++k)
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, firstSubresource + k * (resolveFormat != DXGI_FORMAT_UNKNOWN ? 0 : desc.MipLevels), 1, total, &fp, nullptr, nullptr, &bytes);
        r.footprints.push_back(fp);
        total = (fp.Offset + bytes + 511) & ~511ull;
    }
    r.buffer = CreateBuffer(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT k = 0; k < count; ++k)
    {
        // Consecutive slices of one mip are a mip chain apart.
        const UINT subresource = firstSubresource + k * (resolveFormat != DXGI_FORMAT_UNKNOWN ? 0 : desc.MipLevels);
        if (source == texture)
            Transition(list, texture, subresource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        src.SubresourceIndex = subresource;
        D3D12_TEXTURE_COPY_LOCATION dst{r.buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = r.footprints[k];
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        if (source == texture)
            Transition(list, texture, subresource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    }
    pending.push_back(std::move(r));
}

void ExecuteAndWait(ID3D12CommandQueue* queue, ID3D12CommandList* const* lists, UINT count)
{
    FlushUploads();
    queue->ExecuteCommandLists(count, lists);
    Wait(queue);
    PrintDebugMessages();
    const HRESULT removed = device->GetDeviceRemovedReason();
    if (FAILED(removed))
        Fail("the device was removed while the frame ran", removed);
}

void CompleteReadbacks()
{
    for (Readback& r : pending)
    {
        void* mapped = nullptr;
        if (FAILED(r.buffer->Map(0, nullptr, &mapped)) || !mapped)
        {
            r.note = "not compared: the read-back could not be mapped";
        }
        else
        {
            // Rows out of the copy's pitch into the capture's tight layout.
            for (const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp : r.footprints)
                for (UINT row = 0; row < r.rows; ++row)
                {
                    const uint8_t* at = static_cast<const uint8_t*>(mapped) + fp.Offset + (UINT64)row * fp.Footprint.RowPitch;
                    r.replayed.insert(r.replayed.end(), at, at + r.rowBytes);
                }
            const D3D12_RANGE none{0, 0};
            r.buffer->Unmap(0, &none);
            const size_t size = (size_t)std::min<UINT64>(r.capturedSize, r.replayed.size());
            const uint64_t texels = (uint64_t)r.width * r.height * r.footprints.size();
            const uint32_t texel = texels ? std::max<uint32_t>(1, (uint32_t)(r.capturedSize / texels)) : 1;
            // A 24-bit depth plane is copied as 32 bits whose top byte is undefined.
            const bool d24 = r.aspect == 1 && (r.format == DXGI_FORMAT_D24_UNORM_S8_UINT || r.format == DXGI_FORMAT_R24G8_TYPELESS);
            const uint32_t compared = d24 ? 3 : texel;
            r.compared = true;
            r.texels = size / texel;
            for (size_t t = 0; t + texel <= size; t += texel)
            {
                bool differs = false;
                for (uint32_t k = 0; k < compared; ++k)
                {
                    const uint32_t delta = (uint32_t)std::abs((int)r.captured[t + k] - (int)r.replayed[t + k]);
                    if (delta)
                    {
                        differs = true;
                        r.maxByteDelta = std::max(r.maxByteDelta, delta);
                    }
                }
                if (differs)
                    ++r.differing;
            }
        }
        r.buffer->Release();
        r.buffer = nullptr;
        results.push_back(std::move(r));
    }
    pending.clear();
    for (ID3D12Resource* t : transients)
        t->Release();
    transients.clear();
}

int ReportResults(const std::string& directory, bool writeImages)
{
    size_t identical = 0, differing = 0, skipped = 0;
    if (writeImages && !results.empty())
        std::filesystem::create_directories(directory);
    std::printf("render targets: %zu\n", results.size());
    for (const Readback& r : results)
    {
        if (r.footprints.size() > 1)
            std::printf("  %s (%ux%u, %zu slices): ", r.name.c_str(), r.width, r.height, r.footprints.size());
        else
            std::printf("  %s (%ux%u): ", r.name.c_str(), r.width, r.height);
        if (!r.compared)
        {
            ++skipped;
            std::printf("%s\n", r.note.c_str());
            continue;
        }
        if (r.differing == 0)
        {
            ++identical;
            std::printf("identical to the capture (%llu texels)\n", (unsigned long long)r.texels);
        }
        else
        {
            ++differing;
            std::printf("%llu of %llu texels differ from the capture, largest byte difference %u\n", (unsigned long long)r.differing,
                (unsigned long long)r.texels, r.maxByteDelta);
        }
        if (!writeImages)
            continue;
        const std::string base = directory + "/" + r.name;
        std::ofstream raw(base + "_replayed.raw", std::ios::binary);
        raw.write((const char*)r.replayed.data(), (std::streamsize)r.replayed.size());
        // The PNGs show the first slice; the .raw file holds every one.
        std::vector<uint8_t> a, b;
        if (ToRgba(r, r.captured, (size_t)r.capturedSize, a) && ToRgba(r, r.replayed.data(), r.replayed.size(), b))
        {
            WritePng(base + "_captured.png", r.width, r.height, a);
            WritePng(base + "_replayed.png", r.width, r.height, b);
        }
    }
    if (writeImages && !results.empty())
        std::printf("wrote the targets to %s/\n", directory.c_str());
    if (infoQueue)
        std::printf("debug layer errors: %zu\n", debugErrors);
    return differing == 0 && skipped == 0 ? 0 : 1;
}

// ---- The window

namespace
{

/** The format a swap chain shows a texture of this format in: one of the same family, so a copy is all it takes. */
DXGI_FORMAT SwapChainFormat(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

}  // namespace

bool OpenOutputWindow(ID3D12Resource* output, const char* title)
{
    if (!output)
    {
        std::fprintf(stderr, "the frame has no output to show\n");
        return false;
    }
    const D3D12_RESOURCE_DESC desc = output->GetDesc();
    const DXGI_FORMAT format = SwapChainFormat(desc.Format);
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count > 1 || format == DXGI_FORMAT_UNKNOWN)
    {
        std::fprintf(stderr, "the frame's output (DXGI format %u, %u samples) is not something a swap chain can show\n", (unsigned)desc.Format, desc.SampleDesc.Count);
        return false;
    }
    window = OpenFrameWindow(title, (uint32_t)desc.Width, desc.Height);
    if (!window)
    {
        std::fprintf(stderr, "no window could be opened\n");
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width = (UINT)desc.Width;
    sc.Height = desc.Height;
    sc.Format = format;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* created = nullptr;
    const HWND hwnd = static_cast<HWND>(FrameWindowHandle(window));
    if (FAILED(factory->CreateSwapChainForHwnd(supportQueue, hwnd, &sc, nullptr, nullptr, &created)) ||
        FAILED(created->QueryInterface(IID_PPV_ARGS(&swapChain))))
    {
        if (created)
            created->Release();
        CloseFrameWindow(window);
        window = nullptr;
        std::fprintf(stderr, "no swap chain could be made for the window\n");
        return false;
    }
    created->Release();
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);   // the swap chain is one size
    windowTitle = title ? title : "";
    titleTime = std::chrono::steady_clock::now();
    return true;
}

void SetOutputVsync(bool on) { vsync = on; }

bool PresentOutput(ID3D12Resource* output, D3D12_RESOURCE_STATES state)
{
    if (!window || !swapChain)
        return false;
    if (!PumpFrameWindow(window))
        return false;
    ID3D12Resource* backBuffer = nullptr;
    DX_CHECK(swapChain->GetBuffer(swapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer)));
    // The first mip of the first slice, from the state the frame leaves it in and back.
    ID3D12GraphicsCommandList* list = BeginOneTime();
    Transition(list, output, 0, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list, backBuffer, 0, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{backBuffer, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(list, backBuffer, 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Transition(list, output, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    EndOneTime(list);
    backBuffer->Release();
    const HRESULT presented = swapChain->Present(vsync ? 1 : 0, 0);
    if (FAILED(presented))
        Fail("IDXGISwapChain::Present", presented);
    PrintDebugMessages();

    // The frame rate in the title, twice a second.
    ++framesShown;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - titleTime).count();
    if (seconds >= 0.5)
    {
        char text[320];
        std::snprintf(text, sizeof(text), "%s - %.1f fps", windowTitle.c_str(), (double)(framesShown - framesAtTitle) / seconds);
        SetFrameWindowTitle(window, text);
        framesAtTitle = framesShown;
        titleTime = now;
    }
    return true;
}

int CloseOutputWindow()
{
    if (supportQueue)
        Wait(supportQueue);
    PrintDebugMessages();
    std::printf("frames shown: %llu\n", (unsigned long long)framesShown);
    if (infoQueue)
        std::printf("debug layer errors: %zu\n", debugErrors);
    if (swapChain)
        swapChain->Release();
    swapChain = nullptr;
    if (window)
        CloseFrameWindow(window);
    window = nullptr;
    return debugErrors ? 1 : 0;
}

void DestroySupport()
{
    FlushUploads();
    if (supportQueue)
        Wait(supportQueue);
    for (UploadChunk& c : uploadChunks)
        c.buffer->Release();
    uploadChunks.clear();
    if (uploadList)
        uploadList->Release();
    if (uploadAllocator)
        uploadAllocator->Release();
    PrintDebugMessages();
    for (ID3D12Object* o : owned)
        o->Release();
    owned.clear();
    if (supportList)
        supportList->Release();
    if (supportAllocator)
        supportAllocator->Release();
    if (supportQueue)
        supportQueue->Release();
    if (fence)
        fence->Release();
    if (infoQueue)
        infoQueue->Release();
    if (device)
        device->Release();
    if (factory)
        factory->Release();
    if (fenceEvent)
        CloseHandle(fenceEvent);
    device = nullptr;
}
