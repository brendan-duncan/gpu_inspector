// Draw-call overlays measured while capturing: where one draw of a render pass landed, as the
// render target tab draws it over the target (highlight drawcall, depth test, wireframe --
// RenderDoc's overlays, and the same three `vkinsp_replay --overlay` measures by replaying a Vulkan
// capture, src/replay/src/overlay.cpp).
//
// Nothing is rebuilt here: the pass is issued again into the application's own command list right
// after it ends, out of the calls the capture kept (pass_record.h), the way overdraw.cpp measures
// every pass. Three runs, each into a count target of the pass's size, each read back:
//   * rasterized -- the draw alone, its own culling, no depth or stencil tests, with the counting
//     pixel shader: every fragment it rasterized, its own overdraw included;
//   * passed -- from a copy of the depth-stencil the pass started with, the pass's earlier draws
//     with their color writes off so they still move depth and stencil, then the draw with its
//     own tests: the fragments that passed them;
//   * wireframe -- the draw alone, filled as lines.
// The three become one byte per pixel (OVERLAY_COVERED, OVERLAY_PASSED, OVERLAY_WIREFRAME in
// draw_overlay.ts), which is what the UI draws over the image.
//
// A draw is named by the pass it is in and its ordinal within that pass, not by the command index
// the UI clicked: the measurement happens while the *next* frame records, and that frame's commands
// are numbered again from the start. The reply carries the command index the draw took in the new
// capture, which is the one the overlay is shown on.
//
// What it cannot see is a fragment the draw's own shader discards: the counting shader does not
// discard, so alpha-tested geometry covers its whole quad. The same is true of the Vulkan overlays.
#include "pass_record.h"

#include "common.h"
#include "formats.h"
#include "hooks.h"
#include "shader_edit.h"
#include "tracker.h"
#include "transport.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace dxinsp
{

namespace
{

constexpr uint32_t kRuns = 5;   // rasterized, passed, wireframe, stencil, back-facing

/** One run of the pass: which draws it issues, and with which copy of their pipelines. */
enum class OverlayMode : uint32_t
{
    Rasterized = 0,
    Passed = 1,
    Wireframe = 2,
    Stencil = 3,
    BackFace = 4
};

/** The pipeline copies this file asks for, beside overdraw's and the pixel history's (VariantKind). */
enum class OverlayVariant : uint64_t
{
    Target = 16,      // the measured draw: the counting pixel shader, no tests, its own culling
    Tested = 17,      // ... with the pass's depth-stencil attached and its own tests
    Wireframe = 18,   // ... no tests, filled as lines
    Silent = 19,      // an earlier draw: its own pixel shader, one count target, no color writes
    Stencil = 20,     // ... the stencil test alone, against the pass's depth-stencil copy
    BackFace = 21,    // ... nothing culled, and a shader that writes only for back faces
};

inline uint64_t OverlayKey(OverlayVariant v, DXGI_FORMAT depthFormat)
{
    return (uint64_t)v | ((uint64_t)(uint32_t)depthFormat << 8);
}

/** One measured draw, held until the capture's lists have run and the counts can be read. */
struct PendingOverlay
{
    uint32_t frame = UINT32_MAX;
    ID3D12GraphicsCommandList* list = nullptr;   // not AddRef'd: only a key for the frame it ran in
    uint64_t listId = 0;
    uint32_t passIndex = 0;
    uint32_t drawIndex = 0;
    uint32_t command = 0;
    std::string method;
    uint32_t width = 0;
    uint32_t height = 0;
    bool measured = false;
    bool depthTested = false;
    bool wireframe = false;
    /** The stencil test alone was drawn, and the run with nothing culled was. */
    bool stencilTested = false;
    bool backFaceTested = false;
    std::string note;
    uint32_t rowPitch = 0;
    ComPtr<ID3D12Resource> staging[kRuns];
    KeepList keep;
};

static_assert(std::is_nothrow_move_constructible_v<PendingOverlay>);

struct OverlayState
{
    std::mutex mutex;
    DrawOverlayRequest request;
    std::vector<PendingOverlay> pending;
};

OverlayState& State()
{
    static OverlayState* s = new OverlayState();
    return *s;
}

/**
 * Issues one run of the pass: the measured draw with the copy its mode asks for, and -- in the
 * Passed run alone -- the draws before it with their color writes off, so the depth and stencil
 * they wrote are there for the measured draw to test against.
 */
class OverlayReplay final : public PassReplay
{
public:
    OverlayReplay(OverlayMode mode, uint32_t drawIndex, DXGI_FORMAT depthFormat, D3D12_CPU_DESCRIPTOR_HANDLE dsv, bool dsvBound,
        const MeasuredPass& pass, PendingOverlay& out)
        : _mode(mode), _target(drawIndex), _depthFormat(depthFormat), _dsv(dsv), _dsvBound(dsvBound), _pass(pass), _out(out) {}

    void SetPipeline(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline) override
    {
        _pipeline = pipeline;
        _list = list;
    }

    void ClearDepthStencil(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_CLEAR_FLAGS flags,
        FLOAT depth, UINT8 stencil, UINT numRects, const D3D12_RECT* rects) override
    {
        // The application clearing the pass's depth-stencil clears the measurement's copy of it,
        // so the draws that follow test against what they tested against in the frame.
        if (_dsvBound && handle.ptr == _pass.depth.handle.ptr)
            list->ClearDepthStencilView(_dsv, flags, depth, stencil, numRects, rects);
    }

    void IssueDraw(ID3D12GraphicsCommandList* list, const std::function<void(ID3D12GraphicsCommandList*)>& draw) override
    {
        const uint32_t index = _drawIndex++;
        const bool measured = index == _target;
        // Only the measured draw is issued, except in the Passed run, where the draws before it
        // have to move the depth and stencil first.
        if (!measured && (_mode != OverlayMode::Passed || index > _target))
            return;
        if (!_pipeline)
        {
            if (measured)
                Note("the draw had no pipeline bound");
            return;
        }
        std::string error;
        ID3D12PipelineState* variant = VariantFor(measured, error);
        if (!variant)
        {
            if (measured)
                Note("the draw's pipeline could not be copied: " + error);
            return;
        }
        list->SetPipelineState(variant);
        if (_kept.insert(variant).second)
            KeepObject(_out.keep, variant);
        draw(list);
        if (measured)
            _issued = true;
    }

    void Skip() override
    {
        const uint32_t index = _drawIndex++;
        // An ExecuteIndirect draws out of a buffer the GPU reads, so it cannot be issued on its own.
        if (index == _target)
            Note("an indirect execution's draws cannot be issued one at a time");
    }

    /** The measured draw was issued in this run. */
    bool issued() const { return _issued; }

private:
    ID3D12PipelineState* VariantFor(bool measured, std::string& error)
    {
        PipelineVariant v;
        v.countFormat = DXGI_FORMAT_R16_FLOAT;
        v.setDepthFormat = true;
        v.depthFormat = _dsvBound ? _depthFormat : DXGI_FORMAT_UNKNOWN;
        v.singleSample = true;
        if (!measured)
        {
            // An earlier draw of the Passed run: its own shaders, writing no color, so what it
            // leaves in the depth-stencil is what the measured draw meets.
            v.disableColorWrites = true;
            return VariantOf(_pipeline, OverlayKey(OverlayVariant::Silent, v.depthFormat), v, error);
        }
        const bool dxil = ShaderEditor::Get().PipelineIsDxil(_pipeline);
        const D3D12_SHADER_BYTECODE* shader = CountingPixelShader(dxil, error);
        if (!shader)
            return nullptr;
        v.pixelShader = shader->pShaderBytecode;
        v.pixelShaderSize = shader->BytecodeLength;
        OverlayVariant kind = OverlayVariant::Target;
        if (_mode == OverlayMode::Passed)
        {
            kind = OverlayVariant::Tested;   // its own depth and stencil state, against the copy
        }
        else if (_mode == OverlayMode::Stencil)
        {
            // The stencil test on its own: the depth test is what the Depth Test overlay answers,
            // and a fragment both would have rejected must not be reported as the stencil's doing.
            v.disableDepth = true;
            v.disableDepthWrite = true;
            kind = OverlayVariant::Stencil;
        }
        else
        {
            v.disableDepth = true;
            v.disableStencil = true;
            if (_mode == OverlayMode::Wireframe)
            {
                v.wireframe = true;
                kind = OverlayVariant::Wireframe;
            }
            else if (_mode == OverlayMode::BackFace)
            {
                // Its own geometry with nothing culled, and a shader that writes only where a
                // back-facing fragment landed.
                const D3D12_SHADER_BYTECODE* back = BackFacePixelShader(dxil, error);
                if (!back)
                    return nullptr;
                v.pixelShader = back->pShaderBytecode;
                v.pixelShaderSize = back->BytecodeLength;
                v.disableCull = true;
                kind = OverlayVariant::BackFace;
            }
        }
        return VariantOf(_pipeline, OverlayKey(kind, v.depthFormat), v, error);
    }

    /** The first reason the draw could not be measured, beside whatever the pass already said. */
    void Note(const std::string& text)
    {
        if (_noted)
            return;
        _noted = true;
        _out.note += (_out.note.empty() ? "" : "; ") + text;
    }

    OverlayMode _mode;
    uint32_t _target;
    DXGI_FORMAT _depthFormat;
    D3D12_CPU_DESCRIPTOR_HANDLE _dsv;
    bool _dsvBound;
    const MeasuredPass& _pass;
    PendingOverlay& _out;
    ID3D12PipelineState* _pipeline = nullptr;
    ID3D12GraphicsCommandList* _list = nullptr;
    uint32_t _drawIndex = 0;
    bool _issued = false;
    bool _noted = false;
    std::unordered_set<ID3D12PipelineState*> _kept;
};

/** The command index of the pass's `drawIndex`-th draw, and the method recorded for it. */
void FindDrawCommand(const ListOps& ops, CommandRecorder* rec, uint32_t drawIndex, uint32_t& command, std::string& method)
{
    uint32_t index = 0;
    for (size_t k = ops.passFirst; k < ops.ops.size(); ++k)
    {
        if (ops.ops[k].key.policy != OpPolicy::Draw)
            continue;
        if (index++ != drawIndex)
            continue;
        command = ops.ops[k].command;
        method = RecordedMethod(rec, ops.ops[k].command);
        return;
    }
}

/** How many draws the pass kept, so a request past the end says so rather than measuring nothing. */
uint32_t DrawCountOf(const ListOps& ops)
{
    uint32_t count = 0;
    for (size_t k = ops.passFirst; k < ops.ops.size(); ++k)
        if (ops.ops[k].key.policy == OpPolicy::Draw)
            count++;
    return count;
}

}  // namespace

void StartDrawOverlay(const DrawOverlayRequest& request)
{
    OverlayState& s = State();
    std::vector<PendingOverlay> pending;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        pending.swap(s.pending);
        s.request = request;
    }
    if (request.enabled)
        Log("draw overlay: following draw %u of pass %u", request.drawIndex, request.passIndex);
}

bool DrawOverlayRequested()
{
    OverlayState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.request.enabled;
}

bool MatchDrawOverlayPass(const MeasuredPass& pass)
{
    OverlayState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.request.enabled && s.request.passIndex == pass.passIndex;
}

void MeasureDrawOverlay(MeasuredPass& pass, CommandRecorder* rec, const ListOps& ops)
{
    DrawOverlayRequest request;
    {
        OverlayState& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        request = s.request;
    }
    if (!request.enabled)
        return;

    ScopedInternal internal;
    ID3D12GraphicsCommandList* list = pass.list;
    ID3D12Device* device = pass.device;
    PendingOverlay m;
    m.list = list;
    m.listId = pass.listId;
    m.passIndex = pass.passIndex;
    m.drawIndex = request.drawIndex;
    m.width = pass.width;
    m.height = pass.height;
    FindDrawCommand(ops, rec, request.drawIndex, m.command, m.method);

    const uint32_t drawCount = DrawCountOf(ops);
    if (!pass.note.empty())
    {
        m.note = pass.note;
    }
    else if (request.drawIndex >= drawCount)
    {
        m.note = "the pass holds " + std::to_string(drawCount) + " draws, and draw " + std::to_string(request.drawIndex) +
            " was asked for: the frame captured now is not the frame the draw was chosen in";
    }
    else if (pass.layered)
    {
        m.note = "a layered pass is not measured";
    }
    if (!m.note.empty())
    {
        OverlayState& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.pending.push_back(std::move(m));
        return;
    }

    // The depth-stencil the tested run runs against: the copy overdraw already takes of the
    // attachment as the pass began (CopyDepthStart), so the earlier draws move that rather than
    // the application's own.
    const bool tests = pass.hasDepth && pass.samples == 1 && pass.depthStart;
    if (pass.hasDepth && !tests)
    {
        m.note = "the pass's depth-stencil attachment could not be copied, so the depth test overlay is not measured";
    }

    const std::vector<const LoggedOp*> before = EffectiveOps(ops.ops, ops.passFirst);
    const uint32_t rowPitch = (uint32_t)(((uint64_t)pass.width * 2 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) /
        D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    m.rowPitch = rowPitch;
    bool any = false;
    for (uint32_t run = 0; run < kRuns; ++run)
    {
        const OverlayMode mode = (OverlayMode)run;
        if (mode == OverlayMode::Passed && !tests)
            continue;
        // The stencil test alone needs a stencil aspect in the pass's depth-stencil copy.
        if (mode == OverlayMode::Stencil && (!tests || !FormatOf(pass.depth.format).stencil))
            continue;

        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_R16_FLOAT;
        ComPtr<ID3D12Resource> target = NewMeasurementTexture(device, DXGI_FORMAT_R16_FLOAT, pass.width, pass.height, 1, false,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        if (!target || !MeasurementDescriptor(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtv))
        {
            m.note += std::string(m.note.empty() ? "" : "; ") + "no memory for the overlay target";
            break;
        }
        {
            D3D12_RENDER_TARGET_VIEW_DESC d{};
            d.Format = DXGI_FORMAT_R16_FLOAT;
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(target.get(), &d, rtv);
        }
        KeepObject(m.keep, target.get());

        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        bool dsvBound = false;
        if ((mode == OverlayMode::Passed || mode == OverlayMode::Stencil) &&
            MeasurementDescriptor(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsv))
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC d{};
            d.Format = pass.depth.format;
            d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            device->CreateDepthStencilView(pass.depthStart.get(), &d, dsv);
            dsvBound = true;
            KeepObject(m.keep, pass.depthStart.get());
        }

        list->OMSetRenderTargets(1, &rtv, FALSE, dsvBound ? &dsv : nullptr);
        const FLOAT zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        list->ClearRenderTargetView(rtv, zero, 0, nullptr);
        if (dsvBound)
        {
            // A real render pass clears its depth-stencil in BeginRenderPass rather than in a
            // command of its own, so the copy is cleared here the way the pass began.
            D3D12_CLEAR_FLAGS flags = (D3D12_CLEAR_FLAGS)0;
            if (pass.depth.beginAccess == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
                flags |= D3D12_CLEAR_FLAG_DEPTH;
            if (pass.depth.stencilBeginAccess == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
                flags |= D3D12_CLEAR_FLAG_STENCIL;
            if (flags)
                list->ClearDepthStencilView(dsv, flags, pass.depth.clearValue.DepthStencil.Depth,
                    pass.depth.clearValue.DepthStencil.Stencil, 0, nullptr);
        }

        OverlayReplay replay(mode, request.drawIndex, pass.depth.format, dsv, dsvBound, pass, m);
        for (const LoggedOp* op : before)
            op->op(list, replay);
        for (size_t k = ops.passFirst; k < ops.ops.size(); ++k)
            ops.ops[k].op(list, replay);
        list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        if (!replay.issued())
            continue;

        ComPtr<ID3D12Resource> staging = NewMeasurementReadback(device, (uint64_t)rowPitch * pass.height);
        if (!staging)
        {
            m.note += std::string(m.note.empty() ? "" : "; ") + "no staging memory for the overlay";
            break;
        }
        Transition(list, target.get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = staging.get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16_FLOAT;
        dst.PlacedFootprint.Footprint.Width = pass.width;
        dst.PlacedFootprint.Footprint.Height = pass.height;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = target.get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        m.staging[run] = std::move(staging);
        if (mode == OverlayMode::Passed)
            m.depthTested = true;
        if (mode == OverlayMode::Wireframe)
            m.wireframe = true;
        if (mode == OverlayMode::Stencil)
            m.stencilTested = true;
        if (mode == OverlayMode::BackFace)
            m.backFaceTested = true;
        any = true;
    }
    m.measured = any;
    if (!any && m.note.empty())
        m.note = "the draw was not issued again: it is not one this pass kept";

    OverlayState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.pending.push_back(std::move(m));
}

void AssignDrawOverlayFrame(ID3D12GraphicsCommandList* list, uint32_t frame)
{
    OverlayState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (PendingOverlay& m : s.pending)
        if (m.list == list && m.frame == UINT32_MAX)
            m.frame = frame;
}

void SendDrawOverlay()
{
    OverlayState& s = State();
    std::vector<PendingOverlay> pending;
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        pending.swap(s.pending);
        enabled = s.request.enabled;
        s.request = DrawOverlayRequest{};
    }
    if (!enabled)
        return;

    // The counts of the three runs, folded into the one byte per pixel the UI draws
    // (OVERLAY_COVERED, OVERLAY_PASSED, OVERLAY_WIREFRAME in draw_overlay.ts).
    constexpr uint8_t kCovered = 1, kPassed = 2, kWireframe = 4, kStencilPassed = 8, kBackFacing = 16;
    for (PendingOverlay& m : pending)
    {
        const size_t pixels = (size_t)m.width * m.height;
        std::vector<uint8_t> mask;
        uint64_t fragments = 0, covered = 0, passed = 0, stencilRejected = 0, backFacing = 0;
        if (m.frame == UINT32_MAX)
        {
            m.measured = false;
            m.note = "the command list was not executed during the capture";
        }
        if (m.measured && pixels)
        {
            mask.assign(pixels, 0);
            for (uint32_t run = 0; run < kRuns; ++run)
            {
                if (!m.staging[run])
                    continue;
                const uint8_t* mapped = nullptr;
                {
                    ScopedInternal internal;
                    void* p = nullptr;
                    if (SUCCEEDED(m.staging[run]->Map(0, nullptr, &p)))
                        mapped = static_cast<const uint8_t*>(p);
                }
                if (!mapped)
                    continue;
                const uint8_t bit = run == 0 ? kCovered : run == 1 ? kPassed
                    : run == 2                                     ? kWireframe
                    : run == 3                                     ? kStencilPassed
                                                                   : kBackFacing;
                for (uint32_t y = 0; y < m.height; ++y)
                {
                    const uint16_t* row = reinterpret_cast<const uint16_t*>(mapped + (uint64_t)y * m.rowPitch);
                    for (uint32_t x = 0; x < m.width; ++x)
                    {
                        const float count = HalfToFloat(row[x]);
                        if (count <= 0.0f)
                            continue;
                        mask[(size_t)y * m.width + x] |= bit;
                        if (run == 0)
                        {
                            fragments += (uint64_t)(count + 0.5f);
                            covered++;
                        }
                        else if (run == 1)
                        {
                            passed++;
                        }
                    }
                }
                ScopedInternal internal;
                m.staging[run]->Unmap(0, nullptr);
            }
        }

        if (m.measured && pixels)
        {
            for (uint8_t& bits : mask)
            {
                if (!m.stencilTested && (bits & kCovered))
                    bits |= kStencilPassed;
                else if (m.stencilTested && (bits & kCovered) && !(bits & kStencilPassed))
                    stencilRejected++;
                // Every pixel of a closed mesh has a back face behind it, so the bit only says
                // something where culling left nothing: a back face landed and no front one did.
                if (!(bits & kBackFacing))
                    continue;
                if (bits & kCovered)
                    bits &= (uint8_t)~kBackFacing;
                else
                    backFacing++;
            }
        }

        JsonWriter w;
        w.BeginObject();
        w.Key("action");
        w.String("CaptureDrawOverlay");
        w.Key("command");
        w.Uint(m.command);
        w.Key("method");
        w.String(m.method);
        w.Key("frame");
        w.Uint(m.frame == UINT32_MAX ? 0 : m.frame);
        w.Key("commandBuffer");
        w.Uint(m.listId);
        w.Key("passIndex");
        w.Uint(m.passIndex);
        w.Key("drawIndex");
        w.Uint(m.drawIndex);
        w.Key("measured");
        w.Boolean(m.measured);
        w.Key("width");
        w.Uint(m.width);
        w.Key("height");
        w.Uint(m.height);
        w.Key("fragments");
        w.Uint(fragments);
        w.Key("pixelsCovered");
        w.Uint(covered);
        w.Key("pixelsPassed");
        w.Uint(m.depthTested ? passed : covered);
        w.Key("pixelsRejected");
        w.Uint(m.depthTested && covered >= passed ? covered - passed : 0);
        w.Key("depthTested");
        w.Boolean(m.depthTested);
        w.Key("wireframe");
        w.Boolean(m.wireframe);
        w.Key("stencilTested");
        w.Boolean(m.stencilTested);
        w.Key("backFaceTested");
        w.Boolean(m.backFaceTested);
        w.Key("pixelsStencilRejected");
        w.Uint(stencilRejected);
        w.Key("pixelsBackFacing");
        w.Uint(backFacing);
        w.Key("size");
        w.Uint(mask.size());
        if (!m.note.empty())
        {
            w.Key("note");
            w.String(m.note);
        }
        w.EndObject();
        Transport::Get().SendJson(std::move(w.str()));

        if (!mask.empty())
        {
            JsonWriter h;
            h.BeginObject();
            h.Key("action");
            h.String("CaptureDrawOverlayData");
            h.Key("command");
            h.Uint(m.command);
            h.Key("commandBuffer");
            h.Uint(m.listId);
            h.Key("size");
            h.Uint(mask.size());
            h.EndObject();
            Transport::Get().SendBinary(std::move(h.str()), mask.data(), mask.size());
        }
        Log("draw overlay: draw %u of pass %u (%s): %llu fragments over %llu pixels%s", m.drawIndex, m.passIndex,
            m.method.c_str(), (unsigned long long)fragments, (unsigned long long)covered,
            m.note.empty() ? "" : (" -- " + m.note).c_str());
    }
}

}  // namespace dxinsp
