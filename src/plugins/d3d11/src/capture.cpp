#include "capture.h"

#include "../gen/d3d11_enums.gen.h"
#include "formats.h"
#include "serialize.h"

#include <gpu_inspector/sdk/transport.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace d3d11insp {

using gpuinsp::sdk::JsonValue;
using gpuinsp::sdk::Server;

// ------------------------------------------------------------------------------------------------
// What a capture holds

struct Recorded {
    uint32_t frame = 0;
    uint64_t stream = 0;
    std::string method;
    std::string args;
    std::string state;
    bool synthetic = false;
    /** ExecuteCommandList: the list's commands, as the children of the command. */
    std::shared_ptr<std::vector<Recorded>> children;
    uint64_t childStream = 0;
};

/** A deferred context's recording: its commands since its last FinishCommandList, and the passes in them. */
struct CommandRecorder {
    std::vector<Recorded> commands;
    uint32_t passes = 0;
    /** The render target read-backs of its passes: (index into g_textures, the pass's index within the list). */
    std::vector<std::pair<size_t, uint32_t>> passTextures;
    /** The recording began before the capture did: what came before is missing. */
    bool partial = false;
};

namespace {

struct TextureCapture {
    uint64_t id = 0;
    uint32_t frame = 0;
    uint64_t context = 0;
    uint32_t passIndex = 0;
    uint32_t attachment = 0;
    std::string format;
    const char* aspect = "color";
    int width = 0, height = 0, depth = 1, layers = 1, mip = 0;
    UINT samples = 1;
    /** "" for a pass attachment, "sampled" for a texture a draw read. */
    std::string kind;
    uint32_t capture = 0;
    std::string error;
    std::vector<uint8_t> data;
};

struct BufferCapture {
    uint32_t id = 0;
    uint64_t buffer = 0;
    uint32_t frame = 0;
    uint64_t context = 0;
    int64_t offset = 0;
    size_t size = 0;
    size_t original = 0;
    std::string error;
    std::vector<uint8_t> data;
};

/** A staging copy made during the frame and mapped once it is over (ResolveCopies). */
struct PendingBuffer {
    size_t index = 0;   // into g_buffers
    ID3D11Buffer* staging = nullptr;
    size_t size = 0;
    ID3D11Device* device = nullptr;
};

struct PendingTexture {
    size_t index = 0;   // into g_textures
    ID3D11Resource* staging = nullptr;
    ID3D11Device* device = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;   // the staging texture's format, typed as the data is decoded
    UINT width = 0, height = 0, depth = 1;
    UINT layers = 1;     // array slices to read (mip 0 of each)
    UINT mips = 1;       // the staging texture's mip count (subresource = mip + slice * mips)
    bool volume = false;
    /** A depth-stencil copy: the depth plane goes to `index`, the stencil plane to `stencilIndex` (or SIZE_MAX). */
    bool depthStencil = false;
    size_t stencilIndex = SIZE_MAX;
};

struct PassQueries {
    Context* context = nullptr;
    uint64_t contextId = 0;
    uint32_t frame = 0;
    uint32_t passIndex = 0;
    bool compute = false;
    ID3D11Query* begin = nullptr;
    ID3D11Query* end = nullptr;
};

std::recursive_mutex g_mutex;
std::atomic<bool> g_recording{false};
std::atomic<bool> g_armed{false};
CaptureOptions g_requested;
CaptureOptions g_options;
uint64_t g_presents = 0;
uint64_t g_startPresent = 0;
uint32_t g_frame = 0;
std::vector<Recorded> g_commands;
std::vector<TextureCapture> g_textures;
std::vector<BufferCapture> g_buffers;
size_t g_bufferBytes = 0;
size_t g_imageBytes = 0;
uint32_t g_nextTextureCapture = 1;
std::map<std::tuple<uint64_t, uint32_t, int64_t, size_t>, uint32_t> g_bufferSeen;
std::map<std::pair<uint64_t, uint32_t>, uint32_t> g_textureSeen;
std::vector<PendingBuffer> g_pendingBuffers;
std::vector<PendingTexture> g_pendingTextures;
std::vector<PassQueries> g_passQueries;
/** The immediate contexts whose disjoint query was begun for this capture. */
std::vector<Context*> g_timedContexts;
/** Command lists finished during a capture: their recordings, until they are executed or destroyed. */
std::unordered_map<const void*, std::shared_ptr<CommandRecorder>> g_commandLists;

// Frame statistics: the present intervals since the last report.
std::chrono::steady_clock::time_point g_lastPresent;
std::chrono::steady_clock::time_point g_lastReport;
double g_sumMs = 0, g_minMs = 0, g_maxMs = 0;
uint32_t g_intervals = 0;

/** The commands that close a run of dispatches (a compute pass); the backend's COMPUTE_PASS_END lists the same. */
const std::unordered_set<std::string_view> kComputePassEnd = {
    "OMSetRenderTargets", "OMSetRenderTargetsAndUnorderedAccessViews", "BeginRenderPass", "EndRenderPass",
    "BeginEvent", "EndEvent", "SetMarker", "ExecuteCommandList", "FinishCommandList", "Present", "Present1",
    "ClearState", "Flush", "Flush1",
};

// ------------------------------------------------------------------------------------------------
// Commands

std::vector<Recorded>& StreamOf(Context* c) {
    if (c && c->deferred) {
        if (!c->recorder) {
            c->recorder = std::make_shared<CommandRecorder>();
            c->recorder->partial = true;   // nothing says when this recording began
        }
        return c->recorder->commands;
    }
    return g_commands;
}

void EndComputePass(Context* c);

void Append(Context* c, const char* method, std::string args, std::string state, bool synthetic) {
    Recorded r;
    r.frame = g_frame;
    r.stream = c ? c->id : 0;
    r.method = method;
    r.args = std::move(args);
    r.state = std::move(state);
    r.synthetic = synthetic;
    StreamOf(c).push_back(std::move(r));
}

// ------------------------------------------------------------------------------------------------
// Staging copies

/** A staging buffer of `size` bytes on `device`, or null. */
ID3D11Buffer* MakeStagingBuffer(ID3D11Device* device, size_t size) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = (UINT)size;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* b = nullptr;
    ScopedInternal internal;
    if (FAILED(device->CreateBuffer(&d, nullptr, &b))) return nullptr;
    return b;
}

/**
 * A range of a buffer copied into a staging buffer of the library's own, to be read once the frame
 * is over; its capture id, which the snapshot names it by. Read once per contents (the buffer's
 * generation) and range.
 */
uint32_t CaptureBuffer(Context* c, ID3D11Buffer* buffer, int64_t offset, size_t size) {
    if (!g_options.captureBuffers || !buffer || size == 0) return 0;
    Object* o = Find(buffer);
    if (!o) return 0;
    if (offset < 0 || (o->size && offset >= (int64_t)o->size)) return 0;
    const size_t original = o->size ? std::min(size, (size_t)(o->size - offset)) : size;
    size = std::min(original, g_options.maxBufferSize);
    const auto key = std::make_tuple(o->id, o->generation, offset, size);
    auto it = g_bufferSeen.find(key);
    if (it != g_bufferSeen.end()) return it->second;
    BufferCapture b;
    b.id = (uint32_t)g_buffers.size() + 1;
    b.buffer = o->id;
    b.frame = g_frame;
    b.context = c->id;
    b.offset = offset;
    b.size = size;
    b.original = original > size ? original : 0;
    if (g_bufferBytes + size > g_options.maxBufferTotal) {
        b.error = "the capture's buffer budget is spent";
    } else if (ID3D11Buffer* staging = MakeStagingBuffer(c->device, size)) {
        D3D11_BOX box{(UINT)offset, 0, 0, (UINT)(offset + size), 1, 1};
        {
            ScopedInternal internal;
            c->ptr->CopySubresourceRegion(staging, 0, 0, 0, 0, buffer, 0, &box);
        }
        PendingBuffer p;
        p.index = g_buffers.size();
        p.staging = staging;
        p.size = size;
        p.device = c->device;
        g_pendingBuffers.push_back(p);
        g_bufferBytes += size;
    } else {
        b.error = "a staging buffer could not be created";
    }
    g_buffers.push_back(std::move(b));
    g_bufferSeen[key] = g_buffers.back().id;
    return g_buffers.back().id;
}

/** The typed format a texture's data is decoded as: the view's own, else the resource's, typed. */
DXGI_FORMAT DecodeFormat(const Object& tex, DXGI_FORMAT viewFormat) {
    const bool depth = (tex.bindFlags & D3D11_BIND_DEPTH_STENCIL) != 0 || IsDepthFormat(viewFormat);
    if (viewFormat != DXGI_FORMAT_UNKNOWN) {
        const DXGI_FORMAT typed = TypedFormat(viewFormat, depth);
        if (FormatOf(typed).protocolName) return typed;
    }
    return TypedFormat(tex.format, depth);
}

/**
 * A texture copied into a staging texture of the library's own: `mip` and `firstSlice`/`slices`
 * of it (a render target's view), or with `mip` UINT_MAX its base mip of every slice (a sampled
 * texture). Multisampled textures are resolved first, which a depth format cannot be. The staging
 * texture is mapped once the frame is over. False with `error` when nothing could be copied.
 */
bool CopyTexture(Context* c, Object& tex, UINT mip, UINT firstSlice, UINT slices, DXGI_FORMAT viewFormat, PendingTexture& p, std::string& error) {
    ID3D11Resource* src = (ID3D11Resource*)tex.ptr;
    ID3D11Device* device = c->device;
    const bool whole = mip == UINT_MAX;
    if (whole) { mip = 0; firstSlice = 0; }
    const bool volume = tex.kind == ObjKind::Texture3D;
    const UINT arraySize = volume ? 1 : std::max(1u, tex.depth);
    if (firstSlice >= arraySize) { error = "the view's slices are outside the texture"; return false; }
    slices = whole ? arraySize : std::min(std::max(1u, slices), arraySize - firstSlice);
    const DXGI_FORMAT typed = DecodeFormat(tex, viewFormat);
    const FormatInfo f = FormatOf(typed);
    if (!f.protocolName) { error = std::string("the inspector cannot decode ") + FormatName(tex.format); return false; }
    const UINT w = std::max(1u, tex.width >> mip), h = std::max(1u, tex.height >> mip);
    const UINT d = volume ? std::max(1u, tex.depth >> mip) : 1;
    p.device = device;
    p.format = typed;
    p.width = w;
    p.height = h;
    p.depth = d;
    p.layers = slices;
    p.mips = 1;
    p.volume = volume;
    p.depthStencil = f.depth || f.stencil;
    ScopedInternal internal;
    ID3D11Resource* source = src;
    ComPtr<ID3D11Resource> resolved;
    UINT sourceMip = mip, sourceFirstSlice = firstSlice, sourceMips = tex.mips;
    if (tex.samples > 1) {
        if (f.depth || f.stencil) { error = "a multisampled depth target cannot be resolved for reading"; return false; }
        if (f.compressed) { error = "a multisampled compressed texture cannot be resolved"; return false; }
        // Resolved into a texture of the capture's, one slice per slice, then copied like any other.
        D3D11_TEXTURE2D_DESC rd{};
        rd.Width = w;
        rd.Height = h;
        rd.MipLevels = 1;
        rd.ArraySize = slices;
        rd.Format = typed;
        rd.SampleDesc.Count = 1;
        rd.Usage = D3D11_USAGE_DEFAULT;
        ID3D11Texture2D* r = nullptr;
        if (FAILED(device->CreateTexture2D(&rd, nullptr, &r))) { error = "the resolve texture could not be created"; return false; }
        resolved.reset(r);
        for (UINT s = 0; s < slices; ++s) {
            c->ptr->ResolveSubresource(r, s, src, D3D11CalcSubresource(mip, firstSlice + s, tex.mips), typed);
        }
        source = r;
        sourceMip = 0;
        sourceFirstSlice = 0;
        sourceMips = 1;
    }
    ID3D11Resource* staging = nullptr;
    if (tex.kind == ObjKind::Texture1D) {
        D3D11_TEXTURE1D_DESC sd{};
        sd.Width = w;
        sd.MipLevels = 1;
        sd.ArraySize = slices;
        sd.Format = tex.samples > 1 ? typed : tex.format;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture1D* t = nullptr;
        if (FAILED(device->CreateTexture1D(&sd, nullptr, &t))) { error = "the staging texture could not be created"; return false; }
        staging = t;
    } else if (volume) {
        D3D11_TEXTURE3D_DESC sd{};
        sd.Width = w;
        sd.Height = h;
        sd.Depth = d;
        sd.MipLevels = 1;
        sd.Format = tex.format;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture3D* t = nullptr;
        if (FAILED(device->CreateTexture3D(&sd, nullptr, &t))) { error = "the staging texture could not be created"; return false; }
        staging = t;
    } else {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = w;
        sd.Height = h;
        sd.MipLevels = 1;
        sd.ArraySize = slices;
        sd.Format = tex.samples > 1 ? typed : tex.format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* t = nullptr;
        if (FAILED(device->CreateTexture2D(&sd, nullptr, &t))) { error = "the staging texture could not be created"; return false; }
        staging = t;
    }
    for (UINT s = 0; s < slices; ++s) {
        c->ptr->CopySubresourceRegion(staging, D3D11CalcSubresource(0, s, 1), 0, 0, 0, source,
                                      D3D11CalcSubresource(sourceMip, sourceFirstSlice + s, sourceMips), nullptr);
    }
    p.staging = staging;
    return true;
}

/** A texture a draw samples (or a UAV it writes), read back once per contents: mip 0 of every slice. */
uint32_t CaptureTexture(Context* c, Object& tex, DXGI_FORMAT viewFormat) {
    if (!g_options.captureImages) return 0;
    const auto key = std::make_pair(tex.id, tex.generation);
    auto it = g_textureSeen.find(key);
    if (it != g_textureSeen.end()) return it->second;
    TextureCapture t;
    t.id = tex.id;
    t.frame = g_frame;
    t.context = c->id;
    t.passIndex = c->pass.open ? c->pass.index : 0;
    t.kind = "sampled";
    t.capture = g_nextTextureCapture++;
    t.width = (int)tex.width;
    t.height = (int)std::max(1u, tex.height);
    const bool volume = tex.kind == ObjKind::Texture3D;
    t.depth = volume ? (int)std::max(1u, tex.depth) : 1;
    t.layers = volume ? 1 : (int)std::max(1u, tex.depth);
    t.samples = tex.samples;
    const DXGI_FORMAT typed = DecodeFormat(tex, viewFormat);
    const FormatInfo f = FormatOf(typed);
    t.format = f.protocolName ? f.protocolName : "VK_FORMAT_UNDEFINED";
    if (f.depth) t.aspect = "depth";
    PendingTexture p;
    p.index = g_textures.size();
    std::string error;
    const size_t bytes = (size_t)RowBytes(f, tex.width) * RowCount(f, std::max(1u, tex.height)) * t.depth * t.layers;
    if (g_imageBytes + bytes > g_options.maxImageTotal) {
        t.error = "the capture's image budget is spent";
    } else if (tex.width == 0) {
        t.error = "the texture has no storage";
    } else if (CopyTexture(c, tex, UINT_MAX, 0, 0, viewFormat, p, error)) {
        g_pendingTextures.push_back(p);
        g_imageBytes += bytes;
    } else {
        t.error = error;
    }
    const uint32_t id = t.capture;
    g_textures.push_back(std::move(t));
    g_textureSeen[key] = id;
    return id;
}

// ------------------------------------------------------------------------------------------------
// Passes

/** Whether the OM state's targets are the ones the open pass draws into. */
bool SameTargets(const Context* c) {
    const PipelineState& s = c->state;
    const OpenPass& p = c->pass;
    if (p.targetCount != s.renderTargetCount || p.depth != s.depthStencil) return false;
    for (UINT i = 0; i < p.targetCount; ++i)
        if (p.targets[i] != s.renderTargets[i]) return false;
    return true;
}

void WriteAttachment(JsonWriter& w, ID3D11View* view, uint32_t attachment, bool depth) {
    Object* v = Find(view);
    Object* r = v ? FindById(v->resourceId) : nullptr;
    w.BeginObject();
    w.Key("attachment"); w.Uint(attachment);
    w.Key("aspect"); w.String(depth ? "depth" : "color");
    w.Key("view"); WriteRef(w, view);
    w.Key("resource"); if (r) w.Ref(r->id, r->type.c_str()); else w.Null();
    if (v) {
        w.Key("mip"); w.Uint(v->mip);
        w.Key("firstSlice"); w.Uint(v->firstSlice);
        w.Key("slices"); w.Uint(v->sliceCount);
        if (v->readOnlyDepth || v->readOnlyStencil) { w.Key("readOnly"); w.Boolean(true); }
    }
    if (r) {
        const DXGI_FORMAT typed = DecodeFormat(*r, v ? v->viewFormat : DXGI_FORMAT_UNKNOWN);
        const FormatInfo f = FormatOf(typed);
        w.Key("format"); w.String(f.protocolName ? f.protocolName : "VK_FORMAT_UNDEFINED");
        w.Key("dxgiFormat"); w.String(FormatName(typed));
        w.Key("width"); w.Uint(std::max(1u, r->width >> (v ? v->mip : 0)));
        w.Key("height"); w.Uint(std::max(1u, r->height >> (v ? v->mip : 0)));
        if (r->samples > 1) { w.Key("samples"); w.Uint(r->samples); }
        if (r->swapChain) { w.Key("swapChain"); w.Ref(r->swapChain, "IDXGISwapChain"); }
    }
    w.EndObject();
}

/** The BeginRenderPass command's arguments: the targets, and what the pass did to them. */
std::string PassArgs(Context* c) {
    const OpenPass& p = c->pass;
    JsonWriter w;
    w.BeginObject();
    w.Key("passIndex"); w.Uint(p.index);
    w.Key("attachments");
    w.BeginArray();
    for (UINT i = 0; i < p.targetCount; ++i) {
        if (p.targets[i]) WriteAttachment(w, p.targets[i], i, false);
    }
    if (p.depth) WriteAttachment(w, p.depth, p.targetCount, true);
    w.EndArray();
    // Cleared before anything drew: what the pass started from does not matter (a load op of CLEAR).
    if (p.cleared) {
        w.Key("cleared");
        w.BeginArray();
        for (UINT i = 0; i < p.targetCount; ++i) if (p.cleared & (1u << i)) w.Uint(i);
        if (p.cleared & (1u << 8)) w.String("depth");
        if (p.cleared & (1u << 9)) w.String("stencil");
        w.EndArray();
    }
    // Discarded: what the pass drew there is thrown away (a store op of DONT_CARE).
    if (p.discarded) {
        w.Key("discarded");
        w.BeginArray();
        for (UINT i = 0; i < p.targetCount; ++i) if (p.discarded & (1u << i)) w.Uint(i);
        if (p.discarded & (1u << 8)) w.String("depth");
        if (p.discarded & (1u << 9)) w.String("stencil");
        w.EndArray();
    }
    w.Key("synthetic"); w.Boolean(true);
    w.EndObject();
    return w.str();
}

/** Copies one target of the open pass into staging: an entry (two for depth-stencil) in g_textures. */
void ReadTarget(Context* c, ID3D11View* view, uint32_t attachment, bool depth) {
    Object* v = Find(view);
    Object* r = v ? FindById(v->resourceId) : nullptr;
    TextureCapture t;
    t.id = r ? r->id : 0;
    t.frame = g_frame;
    t.context = c->id;
    t.passIndex = c->pass.index;
    t.attachment = attachment;
    t.aspect = depth ? "depth" : "color";
    if (!v || !r) {
        t.error = "the target is not a resource the library knows";
        g_textures.push_back(std::move(t));
        return;
    }
    t.mip = (int)v->mip;
    t.width = (int)std::max(1u, r->width >> v->mip);
    t.height = (int)std::max(1u, r->height >> v->mip);
    t.layers = (int)std::max(1u, v->sliceCount);
    t.samples = r->samples;
    const DXGI_FORMAT typed = DecodeFormat(*r, v->viewFormat);
    const FormatInfo f = FormatOf(typed);
    t.format = f.protocolName ? f.protocolName : "VK_FORMAT_UNDEFINED";
    const size_t bytes = (size_t)RowBytes(f, (uint32_t)t.width) * RowCount(f, (uint32_t)t.height) * t.layers;
    PendingTexture p;
    p.index = g_textures.size();
    std::string error;
    if (g_imageBytes + bytes > g_options.maxImageTotal) {
        t.error = "the capture's image budget is spent";
    } else if (!CopyTexture(c, *r, v->mip, v->firstSlice, v->sliceCount, v->viewFormat, p, error)) {
        t.error = error;
    } else {
        g_imageBytes += bytes;
        // The stencil plane is an entry of its own under the same attachment index, one byte per texel.
        if (depth && f.stencil) p.stencilIndex = g_textures.size() + 1;
        g_pendingTextures.push_back(p);
    }
    const bool stencil = depth && f.stencil && t.error.empty();
    if (c->deferred && c->recorder) c->recorder->passTextures.push_back({g_textures.size(), c->pass.index});
    g_textures.push_back(std::move(t));
    if (stencil) {
        TextureCapture s = g_textures.back();
        s.aspect = "stencil";
        if (c->deferred && c->recorder) c->recorder->passTextures.push_back({g_textures.size(), c->pass.index});
        g_textures.push_back(std::move(s));
    }
}

void ReadPassAttachments(Context* c) {
    if (!g_options.captureTextures || c->pass.readBack || c->pass.compute) return;
    c->pass.readBack = true;
    const OpenPass& p = c->pass;
    for (UINT i = 0; i < p.targetCount; ++i) {
        if (p.targets[i]) ReadTarget(c, p.targets[i], i, false);
    }
    if (p.depth) ReadTarget(c, p.depth, p.targetCount, true);
}

// Timings: a disjoint query bracketing the frame's timestamps per immediate context, and a
// timestamp written where each pass begins and ends. Read once the frame is over (PassTimes).

ID3D11Query* MakeQuery(ID3D11Device* device, D3D11_QUERY kind) {
    D3D11_QUERY_DESC d{};
    d.Query = kind;
    ID3D11Query* q = nullptr;
    ScopedInternal internal;
    if (FAILED(device->CreateQuery(&d, &q))) return nullptr;
    return q;
}

ID3D11Query* Timestamp(Context* c) {
    if (!g_options.profilePasses || c->deferred) return nullptr;
    if (!c->disjointQuery) {
        c->disjointQuery = MakeQuery(c->device, D3D11_QUERY_TIMESTAMP_DISJOINT);
        if (!c->disjointQuery) return nullptr;
        ScopedInternal internal;
        c->ptr->Begin(c->disjointQuery);
        g_timedContexts.push_back(c);
    }
    ID3D11Query* q = MakeQuery(c->device, D3D11_QUERY_TIMESTAMP);
    if (!q) return nullptr;
    ScopedInternal internal;
    c->ptr->End(q);
    return q;
}

void EndPassTiming(Context* c) {
    if (!c->pass.beginQuery) return;
    PassQueries q;
    q.context = c;
    q.contextId = c->id;
    q.frame = g_frame;
    q.passIndex = c->pass.index;
    q.compute = c->pass.compute;
    q.begin = c->pass.beginQuery;
    q.end = Timestamp(c);
    g_passQueries.push_back(q);
    c->pass.beginQuery = nullptr;
}

void EndPass(Context* c) {
    if (!c->pass.open) return;
    EndPassTiming(c);
    if (c->pass.compute) {
        c->pass.open = false;
        return;
    }
    ReadPassAttachments(c);
    // What the pass cleared and discarded is known now: its BeginRenderPass says so.
    std::vector<Recorded>& stream = StreamOf(c);
    if (c->pass.beginCommand < stream.size() && stream[c->pass.beginCommand].method == "BeginRenderPass") {
        stream[c->pass.beginCommand].args = PassArgs(c);
    }
    JsonWriter w;
    w.BeginObject();
    w.Key("passIndex"); w.Uint(c->pass.index);
    w.Key("synthetic"); w.Boolean(true);
    w.EndObject();
    Append(c, "EndRenderPass", w.str(), "", true);
    c->pass.open = false;
}

void EndComputePass(Context* c) {
    if (c->pass.open && c->pass.compute) EndPass(c);
}

/** Begins a render pass on the bound targets unless one is open on them; false when nothing is bound. */
bool BeginRenderPassIfNeeded(Context* c) {
    const PipelineState& s = c->state;
    if (c->pass.open && !c->pass.compute && SameTargets(c)) return true;
    EndPass(c);
    if (s.renderTargetCount == 0 && !s.depthStencil) return false;
    OpenPass& p = c->pass;
    p = OpenPass();
    p.open = true;
    p.compute = false;
    p.index = c->nextPassIndex++;
    p.targets = s.renderTargets;
    p.targetCount = s.renderTargetCount;
    p.depth = s.depthStencil;
    p.beginCommand = StreamOf(c).size();
    p.eventDepth = c->eventDepth;
    if (c->deferred && c->recorder) ++c->recorder->passes;
    Append(c, "BeginRenderPass", PassArgs(c), "", true);
    p.beginQuery = Timestamp(c);
    return true;
}

/** A dispatch: inside a render pass it stays there; otherwise a compute pass is open or begins. */
void BeginComputePassIfNeeded(Context* c) {
    if (c->pass.open) return;
    OpenPass& p = c->pass;
    p = OpenPass();
    p.open = true;
    p.compute = true;
    p.index = c->nextComputePassIndex++;
    p.eventDepth = c->eventDepth;
    p.beginQuery = Timestamp(c);
}

// ------------------------------------------------------------------------------------------------
// The state at a draw

UINT FormatBytes(DXGI_FORMAT f) {
    const FormatInfo i = FormatOf(f);
    return i.compressed ? 0 : i.bytes;
}

UINT IndexBytes(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R32_UINT ? 4 : 2;
}

void WriteView(Context* c, JsonWriter& w, ID3D11View* view, bool contents) {
    Object* v = Find(view);
    Object* r = v ? FindById(v->resourceId) : nullptr;
    w.Key("view"); WriteRef(w, view);
    w.Key("resource"); if (r) w.Ref(r->id, r->type.c_str()); else w.Null();
    if (!r) return;
    if (r->kind == ObjKind::Buffer) {
        w.Key("offset"); w.Uint(v->viewOffset);
        w.Key("size"); w.Uint(v->viewSize);
        if (contents) { w.Key("data"); w.Uint(CaptureBuffer(c, (ID3D11Buffer*)r->ptr, (int64_t)v->viewOffset, (size_t)v->viewSize)); }
    } else if (contents) {
        w.Key("capture"); w.Uint(CaptureTexture(c, *r, v->viewFormat));
    }
}

void WriteStage(Context* c, JsonWriter& w, int stage, bool contents) {
    const StageBindings& b = c->state.stages[stage];
    if (!b.shader) return;
    w.Key(StageName(stage));
    w.BeginObject();
    w.Key("shader"); WriteRef(w, b.shader);
    w.Key("constantBuffers");
    w.BeginArray();
    for (UINT i = 0; i < b.constantBuffers.size(); ++i) {
        const ConstantBufferBinding& cb = b.constantBuffers[i];
        if (!cb.buffer) continue;
        Object* o = Find(cb.buffer);
        w.BeginObject();
        w.Key("slot"); w.Uint(i);
        w.Key("buffer"); WriteRef(w, cb.buffer);
        const int64_t offset = (int64_t)cb.first * 16;
        const size_t size = cb.count ? (size_t)cb.count * 16 : (o ? o->size : 0);
        w.Key("offset"); w.Uint((uint64_t)offset);
        w.Key("size"); w.Uint(size);
        if (contents) { w.Key("data"); w.Uint(CaptureBuffer(c, cb.buffer, offset, size)); }
        w.EndObject();
    }
    w.EndArray();
    w.Key("resources");
    w.BeginArray();
    for (UINT i = 0; i < b.resources.size(); ++i) {
        if (!b.resources[i]) continue;
        w.BeginObject();
        w.Key("slot"); w.Uint(i);
        WriteView(c, w, b.resources[i], contents);
        w.EndObject();
    }
    w.EndArray();
    w.Key("samplers");
    w.BeginArray();
    for (UINT i = 0; i < b.samplers.size(); ++i) {
        if (!b.samplers[i]) continue;
        w.BeginObject();
        w.Key("slot"); w.Uint(i);
        w.Key("sampler"); WriteRef(w, b.samplers[i]);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
}

void WriteUavs(Context* c, JsonWriter& w, const char* key, const std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT>& uavs, bool contents) {
    w.Key(key);
    w.BeginArray();
    for (UINT i = 0; i < uavs.size(); ++i) {
        if (!uavs[i]) continue;
        w.BeginObject();
        w.Key("slot"); w.Uint(i);
        WriteView(c, w, uavs[i], contents);
        w.EndObject();
    }
    w.EndArray();
}

void WriteVertexInput(Context* c, JsonWriter& w, const DrawParams& p) {
    const PipelineState& s = c->state;
    w.Key("topology"); w.Enum(ToString_D3D_PRIMITIVE_TOPOLOGY(s.topology), s.topology);
    w.Key("inputLayout"); WriteRef(w, s.inputLayout);
    Object* layout = Find(s.inputLayout);
    // The layout's elements, so the draw's vertex format is at hand without the layout object.
    w.Key("attributes");
    w.BeginArray();
    if (layout) {
        UINT location = 0;
        for (const InputElement& e : layout->elements) {
            w.BeginObject();
            w.Key("location"); w.Uint(location++);
            w.Key("name"); w.String(e.semanticName + (e.semanticIndex ? std::to_string(e.semanticIndex) : std::string()));
            w.Key("slot"); w.Uint(e.slot);
            w.Key("offset"); w.Uint(e.offset);
            const FormatInfo f = FormatOf(e.format);
            w.Key("format"); w.String(f.protocolName ? f.protocolName : "VK_FORMAT_UNDEFINED");
            w.Key("perInstance"); w.Boolean(e.perInstance);
            w.Key("stepRate"); w.Uint(e.stepRate);
            w.EndObject();
        }
    }
    w.EndArray();
    // How far into each slot's buffer the draw reads: for a non-indexed draw the vertices it names,
    // for an indexed one to the end of the buffer (the indices are read after the frame, and the
    // inspector finds the vertices in them), capped by the capture's limit either way.
    w.Key("vertexBuffers");
    w.BeginArray();
    for (UINT slot = 0; slot < s.vertexBuffers.size(); ++slot) {
        const VertexBufferBinding& vb = s.vertexBuffers[slot];
        if (!vb.buffer) continue;
        bool perInstance = false, used = !layout;
        if (layout) {
            for (const InputElement& e : layout->elements) {
                if (e.slot != slot) continue;
                used = true;
                perInstance = perInstance || e.perInstance;
            }
        }
        Object* o = Find(vb.buffer);
        w.BeginObject();
        w.Key("slot"); w.Uint(slot);
        w.Key("buffer"); WriteRef(w, vb.buffer);
        w.Key("stride"); w.Uint(vb.stride);
        w.Key("offset"); w.Uint(vb.offset);
        if (used && o && g_options.captureBuffers) {
            size_t size = o->size > vb.offset ? o->size - vb.offset : 0;
            if (!p.indexed && !p.indirect && !p.drawAuto && vb.stride) {
                const uint64_t last = perInstance ? (uint64_t)p.startInstance + p.instances : (uint64_t)p.start + p.count;
                size = std::min<size_t>(size, (size_t)(last * vb.stride));
            } else if (perInstance && vb.stride && !p.indirect) {
                size = std::min<size_t>(size, (size_t)(((uint64_t)p.startInstance + p.instances) * vb.stride));
            }
            w.Key("data"); w.Uint(CaptureBuffer(c, vb.buffer, vb.offset, size));
        }
        w.EndObject();
    }
    w.EndArray();
    if (s.indexBuffer) {
        w.Key("indexBuffer");
        w.BeginObject();
        w.Key("buffer"); WriteRef(w, s.indexBuffer);
        w.Key("format"); w.Enum(ToString_DXGI_FORMAT(s.indexFormat), s.indexFormat);
        w.Key("offset"); w.Uint(s.indexOffset);
        if (p.indexed && g_options.captureBuffers) {
            Object* o = Find(s.indexBuffer);
            const UINT bytes = IndexBytes(s.indexFormat);
            const int64_t offset = (int64_t)s.indexOffset + (int64_t)p.start * bytes;
            size_t size = (size_t)p.count * bytes;
            if (p.indirect && o) size = o->size > (size_t)offset ? o->size - (size_t)offset : 0;
            w.Key("data"); w.Uint(CaptureBuffer(c, s.indexBuffer, offset, size));
        }
        w.EndObject();
    }
}

void WriteFixedFunction(Context* c, JsonWriter& w) {
    const PipelineState& s = c->state;
    w.Key("renderTargets");
    w.BeginArray();
    for (UINT i = 0; i < s.renderTargetCount; ++i) {
        if (!s.renderTargets[i]) continue;
        w.BeginObject();
        w.Key("slot"); w.Uint(i);
        WriteView(c, w, s.renderTargets[i], false);
        w.EndObject();
    }
    w.EndArray();
    if (s.depthStencil) {
        w.Key("depthStencil");
        w.BeginObject();
        WriteView(c, w, s.depthStencil, false);
        w.EndObject();
    }
    w.Key("rasterizerState"); WriteRef(w, s.rasterizerState);
    w.Key("viewports"); WriteViewports(w, s.viewports.data(), s.viewportCount);
    w.Key("scissors"); WriteRects(w, s.scissors.data(), s.scissorCount);
    w.Key("blend");
    w.BeginObject();
    w.Key("state"); WriteRef(w, s.blendState);
    w.Key("factor"); WriteFloats(w, s.blendFactor, 4);
    w.Key("sampleMask"); w.Uint(s.sampleMask);
    w.EndObject();
    w.Key("depthStencilState");
    w.BeginObject();
    w.Key("state"); WriteRef(w, s.depthStencilState);
    w.Key("stencilRef"); w.Uint(s.stencilRef);
    w.EndObject();
    if (s.predicate) {
        w.Key("predicate"); WriteRef(w, s.predicate);
        w.Key("predicateValue"); w.Boolean(s.predicateValue != 0);
    }
    bool so = false;
    for (ID3D11Buffer* b : s.streamOutput) so = so || b != nullptr;
    if (so) {
        w.Key("streamOutput");
        w.BeginArray();
        for (ID3D11Buffer* b : s.streamOutput) WriteRef(w, b);
        w.EndArray();
    }
}

std::string Snapshot(Context* c, const DrawParams* draw, bool indirect, ID3D11Buffer* argsBuffer, UINT argsOffset) {
    JsonWriter w;
    w.BeginObject();
    w.Key("passIndex"); w.Uint(c->pass.open ? c->pass.index : 0);
    if (draw) {
        w.Key("draw");
        w.BeginObject();
        w.Key("indexed"); w.Boolean(draw->indexed);
        w.Key("count"); w.Uint(draw->count);
        w.Key("first"); w.Uint(draw->start);
        w.Key("baseVertex"); w.Int(draw->baseVertex);
        w.Key("instances"); w.Uint(draw->instances);
        w.Key("firstInstance"); w.Uint(draw->startInstance);
        w.Key("indirect"); w.Boolean(draw->indirect);
        if (draw->drawAuto) { w.Key("auto"); w.Boolean(true); }
        w.EndObject();
        WriteVertexInput(c, w, *draw);
        w.Key("stages");
        w.BeginObject();
        for (int stage = VS; stage < CS; ++stage) WriteStage(c, w, stage, true);
        w.EndObject();
        WriteUavs(c, w, "uavs", c->state.psUavs, true);
        WriteFixedFunction(c, w);
    } else {
        w.Key("stages");
        w.BeginObject();
        WriteStage(c, w, CS, true);
        w.EndObject();
        WriteUavs(c, w, "uavs", c->state.csUavs, true);
    }
    if (indirect && argsBuffer) {
        w.Key("indirectArgs");
        w.BeginObject();
        w.Key("buffer"); WriteRef(w, argsBuffer);
        w.Key("offset"); w.Uint(argsOffset);
        w.Key("data"); w.Uint(CaptureBuffer(c, argsBuffer, argsOffset, draw ? 20 : 12));
        w.EndObject();
    }
    w.EndObject();
    return w.str();
}

// ------------------------------------------------------------------------------------------------
// Reading the copies

/** The immediate context of a device, for mapping (an internal call; the reference is released). */
ID3D11DeviceContext* ImmediateOf(ID3D11Device* device) {
    ID3D11DeviceContext* ctx = nullptr;
    ScopedInternal internal;
    device->GetImmediateContext(&ctx);
    if (ctx) ctx->Release();
    return ctx;
}

/** Repacks the rows of one mapped slice into `out`, tightly; a depth-stencil texel is split into its planes. */
void ReadSlice(const PendingTexture& p, const D3D11_MAPPED_SUBRESOURCE& m, TextureCapture& t, TextureCapture* stencil) {
    const FormatInfo f = FormatOf(p.format);
    const uint64_t rowBytes = RowBytes(f, p.width);
    const uint32_t rows = RowCount(f, p.height);
    const uint8_t* base = (const uint8_t*)m.pData;
    for (UINT z = 0; z < p.depth; ++z) {
        const uint8_t* slice = base + (size_t)z * m.DepthPitch;
        if (!p.depthStencil) {
            for (uint32_t y = 0; y < rows; ++y) t.data.insert(t.data.end(), slice + (size_t)y * m.RowPitch, slice + (size_t)y * m.RowPitch + rowBytes);
            continue;
        }
        // Depth-stencil: the depth plane as the inspector decodes it (24 bits in a DWORD, or a 32-bit
        // float), and the stencil byte on its own.
        const bool d32s8 = p.format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || p.format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS || p.format == DXGI_FORMAT_R32G8X24_TYPELESS;
        for (uint32_t y = 0; y < rows; ++y) {
            const uint8_t* row = slice + (size_t)y * m.RowPitch;
            for (uint32_t x = 0; x < p.width; ++x) {
                if (d32s8) {
                    t.data.insert(t.data.end(), row + x * 8, row + x * 8 + 4);
                    if (stencil) stencil->data.push_back(row[x * 8 + 4]);
                } else if (f.bytes == 4) {
                    t.data.insert(t.data.end(), row + x * 4, row + x * 4 + 4);
                    if (stencil) stencil->data.push_back(row[x * 4 + 3]);
                } else {
                    t.data.insert(t.data.end(), row + x * f.bytes, row + (x + 1) * f.bytes);
                }
            }
        }
    }
}

/**
 * The staging copies made during the frame, mapped now that it is over (the GPU has long finished
 * most of them) and released.
 */
void ResolveCopies() {
    for (const PendingBuffer& p : g_pendingBuffers) {
        BufferCapture& out = g_buffers[p.index];
        ID3D11DeviceContext* ctx = ImmediateOf(p.device);
        D3D11_MAPPED_SUBRESOURCE m{};
        ScopedInternal internal;
        if (ctx && SUCCEEDED(ctx->Map(p.staging, 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
            out.data.assign((const uint8_t*)m.pData, (const uint8_t*)m.pData + p.size);
            ctx->Unmap(p.staging, 0);
        } else {
            out.error = "the staging copy could not be mapped";
        }
        p.staging->Release();
    }
    for (const PendingTexture& p : g_pendingTextures) {
        TextureCapture& out = g_textures[p.index];
        TextureCapture* stencil = p.stencilIndex < g_textures.size() ? &g_textures[p.stencilIndex] : nullptr;
        ID3D11DeviceContext* ctx = ImmediateOf(p.device);
        ScopedInternal internal;
        for (UINT s = 0; s < p.layers && out.error.empty(); ++s) {
            D3D11_MAPPED_SUBRESOURCE m{};
            const UINT sub = D3D11CalcSubresource(0, s, p.mips);
            if (ctx && SUCCEEDED(ctx->Map(p.staging, sub, D3D11_MAP_READ, 0, &m)) && m.pData) {
                ReadSlice(p, m, out, stencil);
                ctx->Unmap(p.staging, sub);
            } else {
                out.error = "the staging copy could not be mapped";
                out.data.clear();
                if (stencil) { stencil->error = out.error; stencil->data.clear(); }
            }
        }
        p.staging->Release();
    }
    g_pendingBuffers.clear();
    g_pendingTextures.clear();
}

// ------------------------------------------------------------------------------------------------
// Sending the capture

/** Waits for a query's result; false when the GPU does not deliver it in time. */
template <class T>
bool QueryResult(ID3D11DeviceContext* ctx, ID3D11Query* q, T& out) {
    for (int i = 0; i < 20000; ++i) {
        const HRESULT hr = ctx->GetData(q, &out, sizeof(T), 0);
        if (hr == S_OK) return true;
        if (FAILED(hr)) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return false;
}

std::string PassTimes() {
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("CapturePassTimings");
    w.Key("timestampPeriodNs"); w.Double(1.0);
    struct Time { const PassQueries* q; double startMs, durationMs; };
    std::vector<Time> times;
    {
        ScopedInternal internal;
        for (Context* c : g_timedContexts) {
            if (!c->disjointQuery) continue;
            c->ptr->End(c->disjointQuery);
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            const bool ok = QueryResult(c->ptr, c->disjointQuery, dj);
            if (!ok || dj.Disjoint || !dj.Frequency) {
                LogAlways("pass timings: the GPU's timestamps were disjoint for context %llu, its passes are not timed", (unsigned long long)c->id);
            } else {
                for (const PassQueries& q : g_passQueries) {
                    if (q.context != c || !q.begin || !q.end) continue;
                    UINT64 begin = 0, end = 0;
                    if (!QueryResult(c->ptr, q.begin, begin) || !QueryResult(c->ptr, q.end, end) || end < begin) continue;
                    times.push_back({&q, (double)begin * 1000.0 / (double)dj.Frequency, (double)(end - begin) * 1000.0 / (double)dj.Frequency});
                }
            }
            c->disjointQuery->Release();
            c->disjointQuery = nullptr;
        }
        for (const PassQueries& q : g_passQueries) {
            if (q.begin) q.begin->Release();
            if (q.end) q.end->Release();
        }
    }
    g_passQueries.clear();
    g_timedContexts.clear();
    double origin = 1e300;
    for (const Time& t : times) origin = std::min(origin, t.startMs);
    w.Key("count"); w.Uint(times.size());
    w.Key("passes");
    w.BeginArray();
    for (const Time& t : times) {
        w.BeginObject();
        w.Key("frame"); w.Uint(t.q->frame);
        w.Key("commandBuffer"); w.Uint(t.q->contextId);
        w.Key("passIndex"); w.Uint(t.q->passIndex);
        w.Key("kind"); w.String(t.q->compute ? "compute" : "render");
        w.Key("startMs"); w.Double(t.startMs - origin);
        w.Key("durationMs"); w.Double(t.durationMs);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return w.str();
}

void WriteCommand(JsonWriter& out, const Recorded& r, size_t index, bool child) {
    out.BeginObject();
    if (!child) { out.Key("index"); out.Uint(index); out.Key("frame"); out.Uint(r.frame); }
    out.Key("method"); out.String(r.method);
    if (!child) { out.Key("object"); out.Ref(r.stream, "ID3D11DeviceContext"); }
    out.Key("args"); out.Raw(r.args.empty() ? std::string("{}") : r.args);
    if (!r.state.empty()) { out.Key("state"); out.Raw(r.state); }
    if (r.children) {
        out.Key("children");
        out.BeginArray();
        out.BeginObject();
        out.Key("commandBuffer"); out.Uint(r.childStream);
        out.Key("commands");
        out.BeginArray();
        for (size_t i = 0; i < r.children->size(); ++i) WriteCommand(out, (*r.children)[i], i, true);
        out.EndArray();
        out.EndObject();
        out.EndArray();
    }
    out.EndObject();
}

void SendCapture() {
    Server& server = Server::Get();
    const uint32_t frames = g_frame;
    {
        JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("CaptureFrameResults");
        w.Key("frame"); w.Uint(g_startPresent);
        w.Key("frames"); w.Uint(std::max<uint32_t>(1, frames));
        w.Key("count"); w.Uint(g_commands.size());
        constexpr size_t kBatch = 1000;
        w.Key("batches"); w.Uint((g_commands.size() + kBatch - 1) / kBatch);
        w.Key("api"); w.String("d3d11");
        w.EndObject();
        server.SendJson(w.str());
        for (size_t start = 0, batch = 0; start < g_commands.size(); start += kBatch, ++batch) {
            JsonWriter out;
            out.BeginObject();
            out.Key("action"); out.String("CaptureFrameCommands");
            out.Key("frame"); out.Uint(g_startPresent);
            out.Key("index"); out.Uint(batch);
            out.Key("commands");
            out.BeginArray();
            for (size_t i = start; i < std::min(g_commands.size(), start + kBatch); ++i) WriteCommand(out, g_commands[i], i, false);
            out.EndArray();
            out.EndObject();
            server.SendJson(out.str());
        }
    }
    {
        JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("CaptureTextureFrames");
        w.Key("count"); w.Uint(g_textures.size());
        w.Key("textures");
        w.BeginArray();
        for (const TextureCapture& t : g_textures) {
            w.BeginObject();
            w.Key("id"); w.Uint(t.id);
            w.Key("frame"); w.Uint(t.frame);
            w.Key("commandBuffer"); w.Uint(t.context);
            w.Key("passIndex"); w.Uint(t.passIndex);
            w.Key("attachment"); w.Uint(t.attachment);
            w.Key("format"); w.String(t.format.empty() ? "VK_FORMAT_UNDEFINED" : t.format);
            w.Key("aspect"); w.String(t.aspect);
            w.Key("width"); w.Int(t.width);
            w.Key("height"); w.Int(t.height);
            w.Key("depth"); w.Int(t.depth);
            w.Key("layers"); w.Int(t.layers);
            w.Key("mip"); w.Int(t.mip);
            w.Key("size"); w.Uint(t.data.size());
            if (t.samples > 1) { w.Key("samples"); w.Uint(t.samples); }
            if (!t.error.empty()) { w.Key("error"); w.String(t.error); }
            if (!t.kind.empty()) {
                w.Key("kind"); w.String(t.kind);
                w.Key("capture"); w.Uint(t.capture);
                w.Key("baseLayer"); w.Uint(0);
            }
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
        server.SendJson(w.str());
        for (const TextureCapture& t : g_textures) {
            if (t.data.empty()) continue;
            JsonWriter h;
            h.BeginObject();
            h.Key("action"); h.String("CaptureTextureData");
            h.Key("id"); h.Uint(t.id);
            h.Key("frame"); h.Uint(t.frame);
            h.Key("commandBuffer"); h.Uint(t.context);
            h.Key("passIndex"); h.Uint(t.passIndex);
            h.Key("attachment"); h.Uint(t.attachment);
            h.Key("aspect"); h.String(t.aspect);
            if (t.capture) { h.Key("capture"); h.Uint(t.capture); }
            h.Key("size"); h.Uint(t.data.size());
            h.EndObject();
            server.SendBinary(h.str(), t.data.data(), t.data.size());
        }
    }
    {
        JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("CaptureBuffers");
        w.Key("count"); w.Uint(g_buffers.size());
        w.Key("buffers");
        w.BeginArray();
        for (const BufferCapture& b : g_buffers) {
            w.BeginObject();
            w.Key("id"); w.Uint(b.id);
            w.Key("buffer"); w.Uint(b.buffer);
            w.Key("frame"); w.Uint(b.frame);
            w.Key("commandBuffer"); w.Uint(b.context);
            w.Key("offset"); w.Int(b.offset);
            w.Key("size"); w.Uint(b.data.size());
            if (b.original) { w.Key("originalSize"); w.Uint(b.original); }
            if (!b.error.empty()) { w.Key("error"); w.String(b.error); }
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
        server.SendJson(w.str());
        for (const BufferCapture& b : g_buffers) {
            if (!b.error.empty() || b.data.empty()) continue;
            server.SendBinary("{\"action\":\"CaptureBufferData\",\"id\":" + std::to_string(b.id) + ",\"size\":" + std::to_string(b.data.size()) + "}",
                              b.data.data(), b.data.size());
        }
    }
    // Sent even when there are no timings: a capture that asked for them is then told there are none.
    if (g_options.profilePasses) server.SendJson(PassTimes());
    server.SendJson("{\"action\":\"CaptureComplete\",\"frame\":" + std::to_string(g_startPresent) + ",\"frames\":" + std::to_string(std::max<uint32_t>(1, frames)) + "}");
    LogAlways("capture of %u frame(s) sent: %zu commands, %zu images (%zu MB), %zu buffers (%zu KB)", frames, g_commands.size(),
              g_textures.size(), g_imageBytes >> 20, g_buffers.size(), g_bufferBytes >> 10);
}

void Reset() {
    for (const PendingBuffer& p : g_pendingBuffers) p.staging->Release();
    for (const PendingTexture& p : g_pendingTextures) p.staging->Release();
    {
        ScopedInternal internal;
        for (const PassQueries& q : g_passQueries) {
            if (q.begin) q.begin->Release();
            if (q.end) q.end->Release();
        }
        for (Context* c : g_timedContexts) {
            if (c->disjointQuery) { c->ptr->End(c->disjointQuery); c->disjointQuery->Release(); c->disjointQuery = nullptr; }
        }
    }
    g_commands.clear();
    g_textures.clear();
    g_buffers.clear();
    g_bufferBytes = 0;
    g_imageBytes = 0;
    g_nextTextureCapture = 1;
    g_bufferSeen.clear();
    g_textureSeen.clear();
    g_passQueries.clear();
    g_timedContexts.clear();
    g_pendingBuffers.clear();
    g_pendingTextures.clear();
    g_commandLists.clear();
    g_frame = 0;
    std::lock_guard slock(State().mutex);
    for (auto& [ptr, ctx] : State().contexts) {
        ctx->pass = OpenPass();
        ctx->nextPassIndex = 0;
        ctx->nextComputePassIndex = 0;
        ctx->recorder.reset();
    }
}

void FrameStats() {
    const auto now = std::chrono::steady_clock::now();
    if (g_lastPresent.time_since_epoch().count()) {
        const double ms = std::chrono::duration<double, std::milli>(now - g_lastPresent).count();
        g_sumMs += ms;
        g_minMs = g_intervals ? std::min(g_minMs, ms) : ms;
        g_maxMs = g_intervals ? std::max(g_maxMs, ms) : ms;
        ++g_intervals;
    }
    g_lastPresent = now;
    if (!g_lastReport.time_since_epoch().count()) g_lastReport = now;
    if (now - g_lastReport < std::chrono::milliseconds(100) || !g_intervals) return;
    if (Server::Get().Connected()) {
        JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("FrameStats");
        w.Key("frame"); w.Uint(g_presents);
        w.Key("frameTimeMs"); w.Double(g_sumMs / g_intervals);
        w.Key("minMs"); w.Double(g_minMs);
        w.Key("maxMs"); w.Double(g_maxMs);
        w.Key("frames"); w.Uint(g_intervals);
        w.Key("frameBoundary"); w.String("present");
        w.EndObject();
        Server::Get().SendJson(w.str());
    }
    g_lastReport = now;
    g_sumMs = 0;
    g_intervals = 0;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// Public

bool Recording() {
    return g_recording.load(std::memory_order_relaxed);
}

void RequestCapture(const JsonValue& msg) {
    std::lock_guard lock(g_mutex);
    CaptureOptions o;
    o.frameCount = (uint32_t)std::max(1.0, msg.GetNumber("frameCount", 1));
    if (const JsonValue* at = msg.Get("atFrame"); at && at->kind == JsonValue::Number) o.atFrame = (int64_t)at->num;
    o.captureTextures = msg.GetBool("captureTextures", true);
    o.captureBuffers = msg.GetBool("captureBuffers", true);
    o.captureImages = msg.GetBool("captureImages", true);
    o.profilePasses = msg.GetBool("profilePasses", false);
    if (msg.Get("maxBufferSize")) o.maxBufferSize = (size_t)msg.GetNumber("maxBufferSize");
    if (msg.Get("maxBufferTotal")) o.maxBufferTotal = (size_t)msg.GetNumber("maxBufferTotal");
    if (msg.Get("maxImageTotal")) o.maxImageTotal = (size_t)msg.GetNumber("maxImageTotal");
    g_requested = o;
    g_armed = true;
    Log("capture armed: %u frame(s)%s", o.frameCount, o.atFrame >= 0 ? " at a given frame" : "");
}

void Record(Context* c, const char* method, std::string args, std::string state) {
    if (!Recording() || !c) return;
    std::lock_guard lock(g_mutex);
    if (!Recording()) return;
    if (kComputePassEnd.count(method)) EndComputePass(c);
    Append(c, method, std::move(args), std::move(state), false);
}

void RecordSynthetic(Context* c, const char* method, std::string args) {
    if (!Recording() || !c) return;
    std::lock_guard lock(g_mutex);
    if (!Recording()) return;
    Append(c, method, std::move(args), "", true);
}

std::string BeforeDraw(Context* c, const DrawParams& p) {
    if (!Recording() || !c) return {};
    std::lock_guard lock(g_mutex);
    if (!Recording()) return {};
    BeginRenderPassIfNeeded(c);
    c->pass.drawn = true;
    return Snapshot(c, &p, p.indirect, p.argsBuffer, p.argsOffset);
}

void AfterDraw(Context* c) {
    if (!c) return;
    std::lock_guard lock(g_mutex);
    // What the draw wrote is new contents: UAVs, stream-output targets (the render targets are read at the pass's end).
    for (ID3D11UnorderedAccessView* u : c->state.psUavs) if (u) if (Object* v = Find(u)) if (Object* r = FindById(v->resourceId)) ++r->generation;
    for (ID3D11Buffer* b : c->state.streamOutput) if (b) if (Object* o = Find(b)) ++o->generation;
    for (UINT i = 0; i < c->state.renderTargetCount; ++i) if (c->state.renderTargets[i]) if (Object* v = Find(c->state.renderTargets[i])) if (Object* r = FindById(v->resourceId)) ++r->generation;
    if (c->state.depthStencil) if (Object* v = Find(c->state.depthStencil)) if (Object* r = FindById(v->resourceId)) ++r->generation;
}

std::string BeforeDispatch(Context* c, bool indirect, ID3D11Buffer* argsBuffer, UINT argsOffset) {
    if (!Recording() || !c) return {};
    std::lock_guard lock(g_mutex);
    if (!Recording()) return {};
    BeginComputePassIfNeeded(c);
    return Snapshot(c, nullptr, indirect, argsBuffer, argsOffset);
}

void AfterDispatch(Context* c) {
    if (!c) return;
    std::lock_guard lock(g_mutex);
    for (ID3D11UnorderedAccessView* u : c->state.csUavs) if (u) if (Object* v = Find(u)) if (Object* r = FindById(v->resourceId)) ++r->generation;
}

void BeforeClear(Context* c, ID3D11View* view, bool depth, bool stencil) {
    if (!c || !view) return;
    if (Object* v = Find(view)) if (Object* r = FindById(v->resourceId)) ++r->generation;
    if (!Recording()) return;
    std::lock_guard lock(g_mutex);
    if (!Recording()) return;
    // A clear of a bound target begins its pass; one before any draw says what the pass starts from.
    const PipelineState& s = c->state;
    int slot = -1;
    for (UINT i = 0; i < s.renderTargetCount; ++i) if (s.renderTargets[i] == view) slot = (int)i;
    const bool isDepth = s.depthStencil == view;
    if (slot < 0 && !isDepth) return;
    if (!BeginRenderPassIfNeeded(c)) return;
    if (c->pass.drawn) return;
    // A scissored clear clears part of the target, which keeps the rest.
    Object* rs = Find(s.rasterizerState);
    if (rs && rs->scissorEnable) return;
    if (slot >= 0) c->pass.cleared |= 1u << slot;
    if (isDepth && depth) c->pass.cleared |= 1u << 8;
    if (isDepth && stencil) c->pass.cleared |= 1u << 9;
}

void BeforeDiscard(Context* c, ID3D11View* view, ID3D11Resource* resource) {
    if (!c) return;
    if (resource) if (Object* r = Find(resource)) ++r->generation;
    if (view) if (Object* v = Find(view)) if (Object* r = FindById(v->resourceId)) ++r->generation;
    if (!Recording()) return;
    std::lock_guard lock(g_mutex);
    if (!Recording() || !c->pass.open || c->pass.compute) return;
    const OpenPass& p = c->pass;
    auto sameResource = [&](ID3D11View* target) {
        if (!target) return false;
        if (view) return target == view;
        Object* v = Find(target);
        return v && resource && v->resource == resource;
    };
    uint32_t bits = 0;
    for (UINT i = 0; i < p.targetCount; ++i) if (sameResource(p.targets[i])) bits |= 1u << i;
    if (sameResource(p.depth)) bits |= (1u << 8) | (1u << 9);
    if (!bits) return;
    // The pass's targets are read before they go.
    ReadPassAttachments(c);
    c->pass.discarded |= bits;
}

void AfterSetRenderTargets(Context* c) {
    if (!c || !Recording()) return;
    std::lock_guard lock(g_mutex);
    if (!Recording()) return;
    if (c->pass.open && !c->pass.compute && !SameTargets(c)) EndPass(c);
}

void EndOpenPass(Context* c) {
    if (!c || !Recording()) return;
    std::lock_guard lock(g_mutex);
    if (Recording()) EndPass(c);
}

void AfterBeginEvent(Context* c) {
    if (c) ++c->eventDepth;
}

void BeforeEndEvent(Context* c) {
    if (!c) return;
    // A pass begun inside the event ends with it, so the command tree nests the one in the other.
    if (Recording() && c->pass.open && c->pass.eventDepth >= c->eventDepth) {
        std::lock_guard lock(g_mutex);
        if (Recording()) EndPass(c);
    }
    if (c->eventDepth > 0) --c->eventDepth;
}

void OnFinishCommandList(Context* c, ID3D11CommandList* list) {
    if (!c) return;
    std::lock_guard lock(g_mutex);
    if (!Recording()) { c->recorder.reset(); return; }
    EndPass(c);
    std::shared_ptr<CommandRecorder> rec = c->recorder ? c->recorder : std::make_shared<CommandRecorder>();
    c->recorder.reset();
    c->nextPassIndex = 0;
    c->nextComputePassIndex = 0;
    if (list) g_commandLists[list] = rec;
}

void OnExecuteCommandList(Context* c, ID3D11CommandList* list, BOOL restoreState) {
    (void)restoreState;
    if (!c || !Recording()) return;
    std::lock_guard lock(g_mutex);
    if (!Recording()) return;
    EndPass(c);
    Args a;
    a.ref("pCommandList", list, "ID3D11CommandList").b("RestoreContextState", restoreState != 0);
    Recorded r;
    r.frame = g_frame;
    r.stream = c->id;
    r.method = "ExecuteCommandList";
    r.args = a.str();
    auto it = g_commandLists.find(list);
    if (it != g_commandLists.end()) {
        std::shared_ptr<CommandRecorder> rec = it->second;
        r.children = std::make_shared<std::vector<Recorded>>(rec->commands);
        Object* lo = Find(list);
        r.childStream = lo ? lo->parent : 0;
        // The list's passes count in this context's sequence, which is how the inspector numbers
        // them once it has inlined the list; their read-backs are filed under this context.
        const uint32_t first = c->nextPassIndex;
        c->nextPassIndex += rec->passes;
        for (auto& [index, local] : rec->passTextures) {
            if (index >= g_textures.size()) continue;
            TextureCapture& t = g_textures[index];
            t.context = c->id;
            t.passIndex = first + local;
            t.frame = g_frame;
        }
        rec->passTextures.clear();
    } else {
        // Recorded before the capture began: nothing of it was seen.
        r.children = std::make_shared<std::vector<Recorded>>();
        Recorded note;
        note.method = "<unrecorded command list>";
        r.children->push_back(note);
        Object* lo = Find(list);
        r.childStream = lo ? lo->parent : 0;
    }
    StreamOf(c).push_back(std::move(r));
}

void OnResourceWritten(ID3D11Resource* resource) {
    if (Object* o = Find(resource)) ++o->generation;
}

void BeforePresent(IDXGISwapChain* swapChain, Context* c, const char* method, std::string args) {
    (void)swapChain;
    if (!c || !Recording()) return;
    std::lock_guard lock(g_mutex);
    if (!Recording()) return;
    EndPass(c);
    Append(c, method, std::move(args), "", false);
}

void AfterPresent(Context* c) {
    std::lock_guard lock(g_mutex);
    ++g_presents;
    FrameStats();
    if (c) {
        c->nextPassIndex = 0;
        c->nextComputePassIndex = 0;
    }
    if (Recording()) {
        ++g_frame;
        if (g_frame >= g_options.frameCount) {
            g_recording = false;
            ResolveCopies();
            SendCapture();
            Reset();
        }
        return;
    }
    if (g_armed && (g_requested.atFrame < 0 || (int64_t)g_presents >= g_requested.atFrame)) {
        if (!Server::Get().Connected()) {
            g_armed = false;
            return;
        }
        g_armed = false;
        g_options = g_requested;
        Reset();
        g_startPresent = g_presents;
        g_recording = true;
        Log("capture started at frame %llu", (unsigned long long)g_presents);
    }
}

void OnObjectDestroyed(const void* ptr) {
    std::lock_guard lock(g_mutex);
    g_commandLists.erase(ptr);
}

void OnDisconnect() {
    std::lock_guard lock(g_mutex);
    g_armed = false;
    if (g_recording) {
        g_recording = false;
        Reset();
        Log("the inspector went away mid-capture; the capture is dropped");
    }
}

}  // namespace d3d11insp
