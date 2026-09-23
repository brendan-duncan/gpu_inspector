// Pixel history, measured while capturing: one pixel of one render target followed through every
// render pass of the capture that renders to it, the way vkinsp_replay --pixel follows one through
// a replayed Vulkan frame (src/replay/src/history.cpp) and the Metal library follows one while it
// captures (src/metal/src/pixel_history.mm). Nothing is replayed here: the library is in the
// process, so the pass is issued again into the application's own command list.
//
// For each pass that renders to the texture:
//   * Before the pass begins, every attachment is copied at the pixel into a texture of the
//     library's own of the same format and size (the "shadows"), so the pass starts from what the
//     application's attachments held.
//   * After the application's pass has ended and the capture has read its render targets back, the
//     shadows are bound and the pixel is read: the pass's start.
//   * Then the pass's kept calls are issued in order into the same list, and at every draw: a
//     one-pixel scissor, and the draw six times under occlusion queries with copies of its pipeline
//     that add one step each -- its primitives with no culling and no tests (covered), with its
//     cull mode (facing), with its own pixel shader (shaded, so a discard shows), with the depth
//     test, with the stencil test, and with both. None of those writes anything. Then the draw
//     itself, with the application's pipeline, and the pixel is read again.
// A clear of the followed attachment inside the pass is an event of its own, since a D3D12 pass has
// no load action: ClearRenderTargetView and ClearDepthStencilView are commands in the pass.
//
// Cull mode and the depth-stencil state are pipeline state in D3D12 rather than encoder state as in
// Metal, so the six steps are six pipeline copies rather than two (shader_edit.h builds them).
//
// Not followed: multisampled and layered passes, an ExecuteIndirect's draws (the arguments are in a
// buffer the GPU reads), and more than 1024 draws in one pass.
#include "pass_record.h"

#include "d3d12_enums.gen.h"
#include "device_info.h"
#include "formats.h"
#include "resources.h"
#include "shader_edit.h"
#include "tracker.h"
#include "transport.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace dxinsp
{

/** A pixel followed through one pass: where it is, and the copies of the attachments it is drawn into. */
struct HistoryPass
{
    /** The resource followed (the request's, or the back buffer this frame renders into). */
    uint64_t texture = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    /** The color attachment the pixel is read from, as an index into MeasuredPass::colors. */
    int target = -1;
    /** Why the pass is not followed. */
    std::string note;
    struct Shadow
    {
        ComPtr<ID3D12Resource> texture;
        D3D12_CPU_DESCRIPTOR_HANDLE view{};
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        /** The application's descriptor handle, so a clear of it is recognized. */
        D3D12_CPU_DESCRIPTOR_HANDLE appHandle{};
        /** A real render pass clears the attachment at its start, so the copy is cleared too. */
        bool clearsDepth = false;
        bool clearsStencil = false;
        bool clearsColor = false;
        D3D12_CLEAR_VALUE clearValue{};
    };
    std::vector<Shadow> colors;
    bool hasDepth = false;
    Shadow depth;
};

namespace
{

constexpr int kVariants = 6;
/** Draws followed per pass: seven draws each, in the application's own command list. */
constexpr uint32_t kMaxDraws = 1024;
/** One event's pixel in the readback buffer: the color at the start, the depth halfway, both 512-aligned. */
constexpr uint32_t kSlotBytes = 1024;
constexpr uint32_t kDepthSlotOffset = 512;

const char* const kCountNames[kVariants] = {"covered", "facing", "shaded", "depthPassed", "stencilPassed", "passed"};

struct PendingEvent
{
    std::string kind;
    std::string method;
    std::string detail;
    uint32_t command = 0;
    uint64_t pipeline = 0;
    bool scissored = false;
    uint32_t testsMeasured = 0;
    int64_t query = -1;   // the first of the draw's six occlusion queries
    int64_t slot = -1;    // where the pixel after the event is in the staging buffer
};

/** One followed pass drawn into a command list, waiting for it to run. */
struct PendingHistory
{
    uint32_t frame = UINT32_MAX;
    ID3D12GraphicsCommandList* list = nullptr;   // not AddRef'd: only a key for the frame it ran in
    uint64_t listId = 0;
    uint32_t passIndex = 0;
    uint64_t texture = 0;
    std::string pixelFormat;
    std::string depthFormat;
    uint32_t colorBytes = 0;
    uint32_t depthBytes = 0;
    std::vector<PendingEvent> events;
    std::vector<std::string> notes;
    ComPtr<ID3D12Resource> pixels;    // the texels after each event
    ComPtr<ID3D12Resource> counts;    // the occlusion query results, eight bytes each
    KeepList keep;
};

// As for a measurement (overdraw.cpp): the pending list must not let go of one of the
// application's objects while it grows, since a release comes back through the Release hook.
static_assert(std::is_nothrow_move_constructible_v<PendingHistory>);

std::mutex g_mutex;
PixelHistoryRequest g_request;
/** The request names a swap chain's back buffer (or nothing alive): whichever one this frame renders into is followed. */
bool g_anyBackBuffer = false;
std::string g_device;
std::vector<PendingHistory> g_pending;

/** The protocol's name and packed size for a texel of the followed attachment. */
struct TexelFormat
{
    const char* name = nullptr;
    uint32_t bytes = 0;
    DXGI_FORMAT copyFormat = DXGI_FORMAT_UNKNOWN;
};

TexelFormat ColorTexel(DXGI_FORMAT format)
{
    TexelFormat t;
    const DXGI_FORMAT typed = TypedFormat(format, false);
    const FormatInfo info = FormatOf(typed);
    if (!info.protocolName || info.blockWidth != 1 || info.blockHeight != 1)
        return t;
    t.name = info.protocolName;
    t.bytes = info.bytes;
    t.copyFormat = typed;
    return t;
}

TexelFormat DepthTexel(DXGI_FORMAT format)
{
    TexelFormat t;
    const DXGI_FORMAT typed = TypedFormat(format, true);
    const FormatInfo info = FormatOf(typed);
    if (!info.protocolName || !info.depth)
        return t;
    t.name = info.protocolName;
    // The depth plane copies as its depth format alone: four bytes for D32 and D24, two for D16,
    // which is how the UI sizes the depth aspect of a combined format (capture.cpp does the same).
    t.bytes = typed == DXGI_FORMAT_D16_UNORM ? 2 : 4;
    t.copyFormat = DepthCopyFormat(typed);
    return t;
}

/** The GPU the measurement ran on, for the history's `device`. */
std::string DeviceName(ID3D12Device* device)
{
    IDXGIAdapter* adapter = AdapterOf(device);
    if (!adapter)
        return std::string();
    ScopedInternal internal;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc)))
        return std::string();
    return Narrow(desc.Description, wcsnlen(desc.Description, 128));
}

std::string Hex(const uint8_t* bytes, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i)
    {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}

/** The subresource of an attachment in the application's resource. */
uint32_t SubresourceOfAttachment(const D3D12_RESOURCE_DESC& desc, const PassAttachment& a, uint32_t plane)
{
    const uint32_t mips = std::max<uint32_t>(1, desc.MipLevels);
    const uint32_t slices = std::max<uint32_t>(1, desc.DepthOrArraySize);
    return a.mip + a.slice * mips + plane * mips * slices;
}

/**
 * The pixel history's list, as the kept calls see it: the state they set is issued as the
 * application made it, the scissor is noted but kept at the pixel, and a draw is handed back rather
 * than issued, so the measurement can issue it as many times as it needs.
 */
class HistoryReplay final : public PassReplay
{
public:
    explicit HistoryReplay(HistoryPass& history) : _history(history) {}

    ID3D12PipelineState* pipeline = nullptr;
    bool hasScissor = false;
    D3D12_RECT scissor{};
    std::function<void(ID3D12GraphicsCommandList*)> draw;
    bool skipped = false;
    /** A clear of the followed attachment happened, and the pixel should be read after it. */
    bool cleared = false;
    std::string clearDetail;

    void SetPipeline(ID3D12GraphicsCommandList* list, ID3D12PipelineState* state) override
    {
        pipeline = state;
        if (state)
            list->SetPipelineState(ShaderEditor::Get().Substitute(state));
    }
    /** Noted only: the history's scissor is the one pixel. */
    void SetScissors(ID3D12GraphicsCommandList*, UINT count, const D3D12_RECT* rects) override
    {
        hasScissor = count > 0 && rects != nullptr;
        if (hasScissor)
            scissor = rects[0];
    }
    void ClearTarget(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE handle, const FLOAT color[4], UINT numRects,
        const D3D12_RECT* rects) override
    {
        for (size_t i = 0; i < _history.colors.size(); ++i)
        {
            const HistoryPass::Shadow& s = _history.colors[i];
            if (!s.texture || s.appHandle.ptr != handle.ptr)
                continue;
            list->ClearRenderTargetView(s.view, color, numRects, rects);
            if ((int)i == _history.target)
            {
                cleared = true;
                char text[96];
                snprintf(text, sizeof(text), "%g, %g, %g, %g", color ? color[0] : 0.0, color ? color[1] : 0.0,
                    color ? color[2] : 0.0, color ? color[3] : 0.0);
                clearDetail = text;
            }
        }
    }
    void ClearDepthStencil(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_CLEAR_FLAGS flags,
        FLOAT depth, UINT8 stencil, UINT numRects, const D3D12_RECT* rects) override
    {
        if (!_history.hasDepth || !_history.depth.texture || _history.depth.appHandle.ptr != handle.ptr)
            return;
        list->ClearDepthStencilView(_history.depth.view, flags, depth, stencil, numRects, rects);
    }
    void IssueDraw(ID3D12GraphicsCommandList*, const std::function<void(ID3D12GraphicsCommandList*)>& d) override { draw = d; }
    void Skip() override { skipped = true; }

private:
    HistoryPass& _history;
};

}  // namespace

// ---------------------------------------------------------------------------------------------

void StartPixelHistory(const PixelHistoryRequest& request)
{
    std::vector<PendingHistory> pending;
    bool anyBackBuffer = false;
    if (request.enabled)
    {
        // A back buffer the next frame does not render into, or a resource the application has
        // released, follows whichever back buffer this frame renders into -- the counterpart of a
        // Metal drawable's texture changing every frame.
        TrackedObject obj;
        if (!request.texture || !Tracker::Get().FindById(request.texture, obj) || obj.type != "ID3D12Resource")
        {
            anyBackBuffer = true;
        }
        else
        {
            ResourceInfo info;
            auto* resource = reinterpret_cast<ID3D12Resource*>(static_cast<uintptr_t>(obj.handle));
            if (!ResourceTracker::Get().Get(resource, info) || info.swapChainBuffer)
                anyBackBuffer = true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        pending.swap(g_pending);
        g_request = request;
        g_anyBackBuffer = anyBackBuffer;
        g_device.clear();
    }
    if (request.enabled)
    {
        Log("pixel history: following pixel (%u, %u) of resource %llu, mip %u, layer %u", request.x, request.y,
            (unsigned long long)request.texture, request.mip, request.layer);
    }
}

int MatchPixelHistoryAttachment(const MeasuredPass& pass)
{
    PixelHistoryRequest request;
    bool anyBackBuffer = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        request = g_request;
        anyBackBuffer = g_anyBackBuffer;
    }
    if (!request.enabled)
        return -1;
    for (size_t i = 0; i < pass.colors.size(); ++i)
    {
        const PassAttachment& a = pass.colors[i];
        if (!a.resource)
            continue;
        const bool same = request.texture != 0 && Tracker::Get().IdOf(a.resource) == request.texture;
        bool backBuffer = false;
        if (!same && anyBackBuffer)
        {
            ResourceInfo info;
            backBuffer = ResourceTracker::Get().Get(a.resource, info) && info.swapChainBuffer;
        }
        if (!same && !backBuffer)
            continue;
        if (a.mip != request.mip || a.slice != request.layer)
            continue;
        return (int)i;
    }
    return -1;
}

void PreparePixelHistory(MeasuredPass& pass, int attachment)
{
    PixelHistoryRequest request;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        request = g_request;
    }
    auto h = std::make_shared<HistoryPass>();
    pass.history = h;
    h->target = attachment;
    h->x = request.x;
    h->y = request.y;
    h->texture = Tracker::Get().IdOf(pass.colors[attachment].resource);
    if (h->x >= pass.width || h->y >= pass.height)
    {
        h->note = "the pixel is outside the pass's render target";
        return;
    }
    if (pass.samples > 1)
    {
        h->note = "a multisampled pass is not followed";
        return;
    }
    if (pass.layered)
    {
        h->note = "a layered pass is not followed";
        return;
    }

    ScopedInternal internal;
    ID3D12GraphicsCommandList* list = pass.list;
    // A copy of an attachment the size of the pass, holding its pixel from before the pass.
    auto shadow = [&](const PassAttachment& a, bool depthStencil, HistoryPass::Shadow& out) -> bool {
        D3D12_RESOURCE_DESC desc{};
        ResourceInfo info;
        if (!ResourceTracker::Get().Get(a.resource, info))
        {
            h->note = "an attachment of the pass is not tracked";
            return false;
        }
        desc = info.desc;
        const uint32_t width = std::max<uint32_t>(1, (uint32_t)(desc.Width >> a.mip));
        const uint32_t height = std::max<uint32_t>(1, desc.Height >> a.mip);
        if (width != pass.width || height != pass.height)
        {
            h->note = "the pass's attachments differ in size";
            return false;
        }
        D3D12_CLEAR_VALUE clear{};
        clear.Format = a.format;
        if (depthStencil)
            clear.DepthStencil.Depth = 1.0f;
        out.texture = NewMeasurementTexture(pass.device, desc.Format, width, height, 1, depthStencil,
            D3D12_RESOURCE_STATE_COPY_DEST, &clear);
        if (!out.texture)
        {
            h->note = "no memory for copies of the pass's attachments";
            return false;
        }
        // Kept on the device until the capture's lists have run: the copy is referenced by the
        // application's list from here on, whether or not the pixel turns out to be followable.
        KeepMeasurementObject(pass.device, out.texture.get());
        out.format = a.format;
        out.appHandle = a.handle;
        out.clearValue = a.clearValue;
        const bool preserve = a.beginAccess == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
        out.clearsColor = !depthStencil && a.beginAccess == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
        out.clearsDepth = depthStencil && a.beginAccess == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
        out.clearsStencil = depthStencil && a.stencilBeginAccess == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
        const D3D12_DESCRIPTOR_HEAP_TYPE type = depthStencil ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (!MeasurementDescriptor(pass.device, type, out.view))
        {
            h->note = "no descriptor for copies of the pass's attachments";
            out.texture.reset();
            return false;
        }
        if (depthStencil)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC d{};
            d.Format = a.format;
            d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            pass.device->CreateDepthStencilView(out.texture.get(), &d, out.view);
        }
        else
        {
            D3D12_RENDER_TARGET_VIEW_DESC d{};
            d.Format = a.format;
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            pass.device->CreateRenderTargetView(out.texture.get(), &d, out.view);
        }
        // The one pixel, as the application's attachment held it before the pass.
        uint32_t planes = 1;
        SubresourceCount(pass.device, desc, &planes);
        for (uint32_t plane = 0; plane < planes; ++plane)
        {
            Transition(list, out.texture.get(), plane, D3D12_RESOURCE_STATE_COPY_DEST,
                depthStencil ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        // A real render pass that clears the attachment starts the copy from its clear value
        // instead, which FollowPixel does once the copies are bound.
        for (uint32_t plane = 0; preserve && plane < planes; ++plane)
        {
            const uint32_t source = SubresourceOfAttachment(desc, a, plane);
            bool known = false;
            D3D12_RESOURCE_STATES state = ResourceTracker::Get().StateIn(list, a.resource, source, &known);
            if (!known)
            {
                state = depthStencil ? (a.readOnlyDepth ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE)
                                     : D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            Transition(list, out.texture.get(), plane,
                depthStencil ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_COPY_DEST);
            Transition(list, a.resource, source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = out.texture.get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = plane;   // one mip, one slice: the plane is the index
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = a.resource;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = source;
            const D3D12_BOX box{h->x, h->y, 0, h->x + 1, h->y + 1, 1};
            list->CopyTextureRegion(&dst, h->x, h->y, 0, &src, &box);
            Transition(list, a.resource, source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            Transition(list, out.texture.get(), plane, D3D12_RESOURCE_STATE_COPY_DEST,
                depthStencil ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        return true;
    };
    h->colors.resize(pass.colors.size());
    for (size_t i = 0; i < pass.colors.size(); ++i)
    {
        if (!shadow(pass.colors[i], false, h->colors[i]))
            return;
    }
    if (pass.hasDepth && shadow(pass.depth, true, h->depth))
        h->hasDepth = true;
}

void FollowPixel(MeasuredPass& pass, CommandRecorder* rec, const ListOps& ops)
{
    HistoryPass& h = *pass.history;
    const std::string where = "command list " + std::to_string(pass.listId) + ", pass " + std::to_string(pass.passIndex);
    PendingHistory out;
    out.list = pass.list;
    out.listId = pass.listId;
    out.passIndex = pass.passIndex;
    out.texture = h.texture;
    auto finish = [&]() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.push_back(std::move(out));
    };
    if (!pass.note.empty())
    {
        out.notes.push_back(where + ": " + pass.note);
        return finish();
    }
    if (!h.note.empty())
    {
        out.notes.push_back(where + ": " + h.note);
        return finish();
    }
    if (rec && rec->type() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        out.notes.push_back(where + ": occlusion queries need a direct command list");
        return finish();
    }
    if (rec && rec->state().appQueryDepth > 0)
    {
        out.notes.push_back(where + ": the application has a query open around the pass, which the measurement's may not nest inside");
        return finish();
    }

    ScopedInternal internal;
    ID3D12GraphicsCommandList* list = pass.list;
    ID3D12Device* device = pass.device;
    for (HistoryPass::Shadow& s : h.colors)
        KeepObject(out.keep, s.texture.get());
    if (h.hasDepth)
        KeepObject(out.keep, h.depth.texture.get());

    // The format a placed footprint may name is the plane's own, which for a depth-stencil
    // resource is neither the resource's nor the view's (a planar R32G8X24 or R24G8 format in a
    // footprint is rejected outright). The runtime answers for it.
    auto planeFormat = [&](ID3D12Resource* texture, DXGI_FORMAT fallback) {
        if (!texture)
            return fallback;
        D3D12_RESOURCE_DESC desc = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
        return footprint.Footprint.Format != DXGI_FORMAT_UNKNOWN ? footprint.Footprint.Format : fallback;
    };
    const TexelFormat color = ColorTexel(h.colors[h.target].format);
    if (!color.name)
    {
        out.notes.push_back(where + std::string(": the render target's format ") + FormatName(h.colors[h.target].format) + " cannot be read back");
    }
    out.pixelFormat = color.name ? color.name : "";
    out.colorBytes = color.bytes;
    TexelFormat depth = h.hasDepth ? DepthTexel(h.depth.format) : TexelFormat();
    if (depth.bytes)
        depth.copyFormat = planeFormat(h.depth.texture.get(), depth.copyFormat);
    out.depthFormat = depth.name ? depth.name : "";
    out.depthBytes = depth.bytes;

    uint32_t draws = 0;
    for (size_t k = ops.passFirst; k < ops.ops.size(); ++k)
        if (ops.ops[k].key.policy == OpPolicy::Draw)
            draws++;
    if (draws > kMaxDraws)
    {
        out.notes.push_back(where + ": only the first " + std::to_string(kMaxDraws) + " of its " + std::to_string(draws) + " draws are followed");
        draws = kMaxDraws;
    }
    // One slot per event: the pass's start, every clear of the followed attachment, every draw.
    const uint32_t slots = draws * 2 + 2;
    out.pixels = NewMeasurementReadback(device, (uint64_t)slots * kSlotBytes);
    ComPtr<ID3D12QueryHeap> queries;
    if (draws)
    {
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        qd.Count = draws * kVariants;
        if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(queries.put()))))
            queries.reset();
        out.counts = NewMeasurementReadback(device, (uint64_t)draws * kVariants * 8);
    }
    if (!out.pixels || (draws && (!queries || !out.counts)))
    {
        out.notes.push_back(where + ": no memory for the pixel's values");
        return finish();
    }

    // The shadows, in place of the application's attachments.
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
    for (const HistoryPass::Shadow& s : h.colors)
        rtvs.push_back(s.view);
    list->OMSetRenderTargets((UINT)rtvs.size(), rtvs.empty() ? nullptr : rtvs.data(), FALSE,
        h.hasDepth ? &h.depth.view : nullptr);

    for (const HistoryPass::Shadow& s : h.colors)
    {
        if (s.clearsColor)
            list->ClearRenderTargetView(s.view, s.clearValue.Color, 0, nullptr);
    }
    if (h.hasDepth && (h.depth.clearsDepth || h.depth.clearsStencil))
    {
        D3D12_CLEAR_FLAGS flags = (D3D12_CLEAR_FLAGS)0;
        if (h.depth.clearsDepth)
            flags |= D3D12_CLEAR_FLAG_DEPTH;
        if (h.depth.clearsStencil)
            flags |= D3D12_CLEAR_FLAG_STENCIL;
        list->ClearDepthStencilView(h.depth.view, flags, h.depth.clearValue.DepthStencil.Depth,
            h.depth.clearValue.DepthStencil.Stencil, 0, nullptr);
    }

    uint32_t slot = 0;
    auto readPixel = [&]() {
        const uint64_t base = (uint64_t)slot * kSlotBytes;
        auto copy = [&](const HistoryPass::Shadow& s, const TexelFormat& format, uint64_t offset, bool depthStencil) {
            if (!format.bytes || !s.texture)
                return;
            Transition(list, s.texture.get(), 0,
                depthStencil ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = out.pixels.get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = offset;
            dst.PlacedFootprint.Footprint.Format = format.copyFormat;
            dst.PlacedFootprint.Footprint.Width = 1;
            dst.PlacedFootprint.Footprint.Height = 1;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = s.texture.get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            const D3D12_BOX box{h.x, h.y, 0, h.x + 1, h.y + 1, 1};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
            Transition(list, s.texture.get(), 0, D3D12_RESOURCE_STATE_COPY_SOURCE,
                depthStencil ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
        };
        copy(h.colors[h.target], color, base, false);
        if (h.hasDepth)
            copy(h.depth, depth, base + kDepthSlotOffset, true);
        return (int64_t)slot++;
    };

    PendingEvent load;
    load.kind = "load";
    load.command = pass.beginCommand;
    load.method = RecordedMethod(rec, pass.beginCommand);
    // OMSetRenderTargets has no load action, so the attachment keeps what it held; BeginRenderPass
    // says what it does with it.
    if (const char* access = ToString_D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE(pass.colors[h.target].beginAccess))
        load.detail = access;
    load.slot = readPixel();
    out.events.push_back(load);

    const D3D12_RECT pixelRect{(LONG)h.x, (LONG)h.y, (LONG)h.x + 1, (LONG)h.y + 1};
    const DXGI_FORMAT shadowDepthFormat = h.hasDepth ? h.depth.format : DXGI_FORMAT_UNKNOWN;
    std::string copyError;
    bool noteIndirect = false;
    uint32_t drawIndex = 0;
    HistoryReplay replay(h);
    for (const LoggedOp* op : EffectiveOps(ops.ops, ops.passFirst))
        op->op(list, replay);
    for (size_t k = ops.passFirst; k < ops.ops.size(); ++k)
    {
        const LoggedOp& logged = ops.ops[k];
        if (logged.key.policy != OpPolicy::Draw)
        {
            replay.cleared = false;
            logged.op(list, replay);
            if (replay.cleared)
            {
                PendingEvent clear;
                clear.kind = "clear";
                clear.command = logged.command;
                clear.method = RecordedMethod(rec, logged.command);
                clear.detail = replay.clearDetail;
                clear.slot = readPixel();
                out.events.push_back(std::move(clear));
                replay.cleared = false;
            }
            continue;
        }
        if (drawIndex >= draws)
            break;
        PendingEvent e;
        e.kind = "draw";
        e.command = logged.command;
        e.method = RecordedMethod(rec, logged.command);
        replay.draw = nullptr;
        replay.skipped = false;
        logged.op(list, replay);   // keeps the draw rather than issuing it
        e.pipeline = replay.pipeline ? Tracker::Get().IdOf(replay.pipeline) : 0;
        const bool inside = !replay.hasScissor || ((LONG)h.x >= replay.scissor.left && (LONG)h.x < replay.scissor.right && (LONG)h.y >= replay.scissor.top && (LONG)h.y < replay.scissor.bottom);
        if (replay.skipped)
        {
            noteIndirect = true;
        }
        else if (!inside)
        {
            e.scissored = true;
        }
        else if (replay.draw && replay.pipeline)
        {
            const uint32_t base = drawIndex * kVariants;
            e.query = (int64_t)base;
            list->RSSetScissorRects(1, &pixelRect);
            std::string error;
            const bool dxil = ShaderEditor::Get().PipelineIsDxil(replay.pipeline);
            const D3D12_SHADER_BYTECODE* cover = CoverPixelShader(dxil, error);
            if (!cover && copyError.empty())
                copyError = error;
            for (int v = 0; v < kVariants && cover; ++v)
            {
                PipelineVariant pv;
                // Each step adds one thing to the last: coverage, culling, the pixel shader, the
                // depth test, the stencil test, both.
                if (v < 2)
                {
                    pv.pixelShader = cover->pShaderBytecode;
                    pv.pixelShaderSize = cover->BytecodeLength;
                }
                pv.disableCull = v == 0;
                pv.disableDepth = v != 3 && v != 5;
                pv.disableStencil = v != 4 && v != 5;
                pv.disableDepthWrite = true;
                pv.disableStencilWrites = true;
                pv.disableColorWrites = true;
                pv.setDepthFormat = true;
                pv.depthFormat = shadowDepthFormat;
                // Cover, Facing, Shaded, Depth, Stencil, Both, in that order.
                ID3D12PipelineState* variant =
                    VariantOf(replay.pipeline, VariantKey((VariantKind)(1 + v), shadowDepthFormat), pv, error);
                if (!variant)
                {
                    if (copyError.empty())
                        copyError = error;
                    continue;
                }
                KeepObject(out.keep, variant);
                list->SetPipelineState(variant);
                list->BeginQuery(queries.get(), D3D12_QUERY_TYPE_OCCLUSION, base + v);
                replay.draw(list);
                list->EndQuery(queries.get(), D3D12_QUERY_TYPE_OCCLUSION, base + v);
                e.testsMeasured |= 1u << v;
            }
            // The draw itself, with the application's own pipeline, into the copies.
            list->SetPipelineState(ShaderEditor::Get().Substitute(replay.pipeline));
            replay.draw(list);
            KeepObject(out.keep, replay.pipeline);
        }
        e.slot = readPixel();
        out.events.push_back(std::move(e));
        drawIndex++;
    }
    if (drawIndex && queries && out.counts)
    {
        list->ResolveQueryData(queries.get(), D3D12_QUERY_TYPE_OCCLUSION, 0, drawIndex * kVariants, out.counts.get(), 0);
        KeepObject(out.keep, queries.get());
    }
    list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    if (noteIndirect)
    {
        out.notes.push_back(where + ": an ExecuteIndirect's draws are not followed, so the values after it may be missing its writes");
    }
    if (!copyError.empty())
        out.notes.push_back(where + ": some draws were not measured: " + copyError);
    if (g_device.empty())
    {
        std::string name = DeviceName(device);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_device.empty())
            g_device = std::move(name);
    }
    finish();
}

void AssignPixelHistoryFrame(ID3D12GraphicsCommandList* list, uint32_t frame)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (PendingHistory& h : g_pending)
        if (h.list == list && h.frame == UINT32_MAX)
            h.frame = frame;
}

void SendPixelHistory()
{
    PixelHistoryRequest request;
    std::vector<PendingHistory> pending;
    std::string device;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        request = g_request;
        pending.swap(g_pending);
        device = g_device;
        g_request = PixelHistoryRequest();
    }
    if (!request.enabled)
        return;

    uint64_t texture = 0;
    std::string pixelFormat;
    std::string depthFormat;
    for (const PendingHistory& h : pending)
    {
        if (!texture)
            texture = h.texture;
        if (pixelFormat.empty())
            pixelFormat = h.pixelFormat;
        if (depthFormat.empty())
            depthFormat = h.depthFormat;
    }

    JsonWriter w;
    w.BeginObject();
    w.Key("action");
    w.String("CapturePixelHistory");
    w.Key("history");
    w.BeginObject();
    w.Key("format");
    w.String("gpu-inspector-pixel-history");
    w.Key("version");
    w.Uint(1);
    w.Key("device");
    w.String(device);
    w.Key("image");
    w.Uint(texture ? texture : request.texture);
    w.Key("requestedImage");
    w.Uint(request.texture);
    w.Key("x");
    w.Uint(request.x);
    w.Key("y");
    w.Uint(request.y);
    w.Key("mip");
    w.Uint(request.mip);
    w.Key("layer");
    w.Uint(request.layer);
    w.Key("pixelFormat");
    w.String(pixelFormat);
    w.Key("depthFormat");
    w.String(depthFormat);
    w.Key("events");
    w.BeginArray();
    size_t events = 0;
    std::vector<std::string> notes;
    for (PendingHistory& h : pending)
    {
        for (std::string& n : h.notes)
            notes.push_back(std::move(n));
        const uint8_t* pixels = nullptr;
        const uint8_t* counts = nullptr;
        {
            ScopedInternal internal;
            void* p = nullptr;
            if (h.pixels && SUCCEEDED(h.pixels->Map(0, nullptr, &p)))
                pixels = static_cast<const uint8_t*>(p);
            void* c = nullptr;
            if (h.counts && SUCCEEDED(h.counts->Map(0, nullptr, &c)))
                counts = static_cast<const uint8_t*>(c);
        }
        for (const PendingEvent& e : h.events)
        {
            w.BeginObject();
            w.Key("kind");
            w.String(e.kind);
            w.Key("command");
            w.Uint(e.command);
            w.Key("method");
            w.String(e.method);
            w.Key("detail");
            w.String(e.detail);
            w.Key("commandBuffer");
            w.Uint(h.listId);
            w.Key("frame");
            w.Uint(h.frame == UINT32_MAX ? 0 : h.frame);
            w.Key("passIndex");
            w.Uint(h.passIndex);
            w.Key("pipeline");
            w.Uint(e.pipeline);
            w.Key("scissored");
            w.Boolean(e.scissored);
            w.Key("testsMeasured");
            w.Uint(e.testsMeasured);
            for (int v = 0; v < kVariants; ++v)
            {
                uint64_t value = 0;
                if (counts && e.query >= 0 && (e.testsMeasured & (1u << v)))
                {
                    memcpy(&value, counts + (size_t)(e.query + v) * 8, 8);
                }
                w.Key(kCountNames[v]);
                w.Uint(value);
            }
            const bool read = pixels != nullptr && e.slot >= 0;
            w.Key("value");
            w.String(read && h.colorBytes ? Hex(pixels + (size_t)e.slot * kSlotBytes, h.colorBytes) : std::string());
            w.Key("depth");
            w.String(read && h.depthBytes ? Hex(pixels + (size_t)e.slot * kSlotBytes + kDepthSlotOffset, h.depthBytes)
                                          : std::string());
            w.EndObject();
            events++;
        }
        ScopedInternal internal;
        if (pixels)
            h.pixels->Unmap(0, nullptr);
        if (counts)
            h.counts->Unmap(0, nullptr);
    }
    w.EndArray();
    w.Key("notes");
    w.BeginArray();
    if (pending.empty())
        w.String("No render pass of the capture rendered to that resource at that mip and layer.");
    for (const std::string& n : notes)
        w.String(n);
    w.EndArray();
    w.Key("problems");
    w.BeginArray();
    w.EndArray();
    w.EndObject();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
    Log("pixel history: %zu event(s) over %zu pass(es) sent", events, pending.size());
}

}  // namespace dxinsp
