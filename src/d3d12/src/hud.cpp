#include "hud.h"

#include "capture.h"
#include "frame_pause.h"
#include "hud_hotkey.h"
#include "hud_shaders.gen.h"
#include "ui_messages.h"

#include <cstring>

namespace dxinsp
{

Hud& Hud::Get()
{
    // DXINSP_HUD is read here rather than at static-init time: the first call is from a present,
    // long after DxinspInitialize has set the configuration.
    static Hud* instance = [] {
        Hud* hud = new Hud();
        // The HUD's capture hotkey, armed with the HUD itself (hud_hotkey.h). A misspelled key is
        // reported here rather than on the frame it fails to do anything.
        const std::string hotkey = ConfigValue("DXINSP_HOTKEY");
        if (!gpuhud::CaptureHotkey::Get().SetBinding(hotkey.c_str()))
            Log("DXINSP_HOTKEY=\"%s\" is not a key this understands (\"F11\", \"CTRL+F9\", \"off\"); "
                "the capture hotkey is off",
                hotkey.c_str());
        if (ConfigFlag("DXINSP_HUD"))
            hud->SetEnabled(true);
        return hud;
    }();
    return *instance;
}

void Hud::SetEnabled(bool on)
{
    const bool was = _enabled.exchange(on, std::memory_order_relaxed);
    // The capture hotkey is armed with the HUD, which is the only thing on the screen that says
    // the key is live (hud_hotkey.h).
    gpuhud::CaptureHotkey::Get().SetEnabled(on);
    if (was != on)
    {
        const char* hotkey = gpuhud::CaptureHotkey::Get().Name();
        Log("in-app HUD %s%s%s", on ? "on" : "off", on && hotkey ? ", capture hotkey " : "",
            on && hotkey ? hotkey : "");
    }
}

void Hud::PollHotkey()
{
    if (!gpuhud::CaptureHotkey::Get().Poll())
        return;
    const char* name = gpuhud::CaptureHotkey::Get().Name();
    char source[64];
    snprintf(source, sizeof(source), "the capture hotkey (%s)", name ? name : "");
    RequestInspectorCapture(0, nullptr, source);
}

// -----------------------------------------------------------------------------------------------
// Setup

Hud::DeviceResources* Hud::Resources(ID3D12Device* device)
{
    auto it = _devices.find(device);
    if (it != _devices.end())
        return it->second.failed ? nullptr : &it->second;

    DeviceResources r;
    ScopedInternal internal;

    auto fail = [&](const char* why) -> DeviceResources* {
        Log("HUD: %s; the HUD will not be drawn on this device", why);
        r.failed = true;
        _devices[device] = std::move(r);
        return nullptr;
    };

    // Two root constants (the reciprocal of the target's size) and nothing else: the HUD samples
    // nothing, so it binds no descriptor table and needs no heap of its own.
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    param.Constants.ShaderRegister = 0;
    param.Constants.RegisterSpace = 0;
    param.Constants.Num32BitValues = 4;   // float2 invTargetSize + the padding the cbuffer declares
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 1;
    rsd.pParameters = &param;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), error.put())))
        return fail("the root signature would not serialize");
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(r.rootSignature.put()))))
        return fail("the root signature would not be created");

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(r.fence.put()))))
        return fail("the fence would not be created");
    r.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!r.event)
        return fail("the fence event would not be created");

    r.frames.resize(4);
    for (auto& f : r.frames)
    {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(f.allocator.put()))))
            return fail("a command allocator would not be created");
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.allocator.get(),
                nullptr, IID_PPV_ARGS(f.list.put()))))
            return fail("a command list would not be created");
        f.list->Close();   // created open; every frame starts with a Reset
    }

    Log("HUD: ready");
    _devices[device] = std::move(r);
    return &_devices[device];
}

Hud::SwapChainResources* Hud::Ensure(ID3D12Device* device, IDXGISwapChain* swapChain, DeviceResources& r)
{
    SwapChainResources& s = _swapChains[swapChain];

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc)))
        return nullptr;
    if (s.usable && s.format == desc.BufferDesc.Format && s.width == desc.BufferDesc.Width &&
        s.height == desc.BufferDesc.Height && s.buffers.size() == desc.BufferCount)
        return &s;

    ScopedInternal internal;
    s = SwapChainResources{};
    s.format = desc.BufferDesc.Format;
    s.width = desc.BufferDesc.Width;
    s.height = desc.BufferDesc.Height;
    if (!s.width || !s.height || s.format == DXGI_FORMAT_UNKNOWN)
        return nullptr;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = desc.BufferCount;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(s.rtvHeap.put()))))
    {
        Log("HUD: the render target view heap would not be created");
        return nullptr;
    }
    const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = s.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < desc.BufferCount; ++i)
    {
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(buffer.put()))))
        {
            Log("HUD: back buffer %u could not be fetched", i);
            return nullptr;
        }
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = s.format;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(buffer.get(), &rtv, handle);
        s.rtvs.push_back(handle);
        s.buffers.push_back(std::move(buffer));
        handle.ptr += stride;
    }

    // One instance per rectangle; the four vertices of the strip come from SV_VertexID.
    D3D12_INPUT_ELEMENT_DESC inputs[2]{};
    inputs[0] = {"RECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1};
    inputs[1] = {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = r.rootSignature.get();
    pso.VS = {kHudVertDxbc, sizeof(kHudVertDxbc)};
    pso.PS = {kHudPixelDxbc, sizeof(kHudPixelDxbc)};
    pso.InputLayout = {inputs, 2};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = s.format;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    auto& blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(s.pipeline.put()))))
    {
        Log("HUD: the pipeline state would not be created for back buffer format %d", (int)s.format);
        return nullptr;
    }

    s.usable = true;
    Log("HUD: drawing into a %ux%u swap chain of %u buffers", s.width, s.height, (uint32_t)s.buffers.size());
    return &s;
}

bool Hud::EnsureVertexBuffer(ID3D12Device* device, Frame& f, uint32_t rects)
{
    if (f.mapped && f.capacity >= rects)
        return true;
    ScopedInternal internal;
    if (f.mapped)
    {
        f.vertices->Unmap(0, nullptr);
        f.mapped = nullptr;
    }
    f.vertices.reset();
    f.capacity = 0;

    uint32_t capacity = 256;
    while (capacity < rects)
        capacity *= 2;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (UINT64)capacity * sizeof(gpuhud::Rect);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(f.vertices.put()))))
        return false;
    // Mapped once and left mapped: an upload heap is CPU-visible, and the rectangles are rewritten
    // every frame.
    D3D12_RANGE none{0, 0};
    if (FAILED(f.vertices->Map(0, &none, reinterpret_cast<void**>(&f.mapped))))
    {
        f.vertices.reset();
        return false;
    }
    f.capacity = capacity;
    return true;
}

// -----------------------------------------------------------------------------------------------
// Timing (the same window as the Vulkan layer's; see src/vulkan/src/hud.cpp)

void Hud::UpdateTiming(DeviceResources& r)
{
    const auto now = std::chrono::steady_clock::now();
    const auto previous = r.lastDraw;
    const uint64_t generation = gpuinsp::FramePause::Get().Generation();
    const bool acrossPause = generation != r.pauseGeneration;
    r.lastDraw = now;
    r.pauseGeneration = generation;
    if (previous.time_since_epoch().count() == 0)
        return;
    if (acrossPause)
        return;   // the interval is the length of a pause, not of a frame
    const double ms = std::chrono::duration<double, std::milli>(now - previous).count();
    if (ms <= 0 || ms > 10000)
        return;

    if (r.windowFrames == 0)
    {
        r.minMs = ms;
        r.maxMs = ms;
    }
    else
    {
        if (ms < r.minMs)
            r.minMs = ms;
        if (ms > r.maxMs)
            r.maxMs = ms;
    }
    r.windowMs += ms;
    r.windowFrames++;
    if (r.smoothedMs == 0)
    {
        r.smoothedMs = ms;
        r.shownMinMs = ms;
        r.shownMaxMs = ms;
    }
    if (r.windowMs >= 500.0)
    {
        r.smoothedMs = r.windowMs / r.windowFrames;
        r.shownMinMs = r.minMs;
        r.shownMaxMs = r.maxMs;
        r.windowMs = 0;
        r.windowFrames = 0;
    }
}

// -----------------------------------------------------------------------------------------------
// Drawing

void Hud::Draw(ID3D12Device* device, IDXGISwapChain* swapChain, ID3D12CommandQueue* queue)
{
    if (!Enabled() || !device || !swapChain || !queue)
        return;

    std::lock_guard<std::mutex> lock(_mutex);
    DeviceResources* res = Resources(device);
    if (!res)
        return;
    DeviceResources& r = *res;
    UpdateTiming(r);
    if (r.smoothedMs <= 0)
        return;

    SwapChainResources* s = Ensure(device, swapChain, r);
    if (!s || !s->usable)
        return;

    // Which back buffer is about to be shown. Only the flip model can say; a blit-model swap chain
    // has one back buffer anyway, so index 0 is right there.
    UINT index = 0;
    {
        ComPtr<IDXGISwapChain3> sc3;
        if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(sc3.put()))) && sc3)
            index = sc3->GetCurrentBackBufferIndex();
    }
    if (index >= s->rtvs.size())
        return;

    gpuhud::HudState state;
    state.frameMs = r.smoothedMs;
    state.minMs = r.shownMinMs;
    state.maxMs = r.shownMaxMs;
    state.frame = CaptureManager::Get().FrameCounter() + 1;   // this frame has not ended yet
    state.paused = gpuinsp::FramePause::Get().Paused();
    state.capturing = CaptureManager::Get().IsCapturing();
    state.backend = "D3D12";
    state.hotkey = gpuhud::CaptureHotkey::Get().Name();
    std::vector<gpuhud::Rect> rects;
    gpuhud::BuildHud(rects, state, s->width, s->height, gpuhud::HudScale(s->width));
    if (rects.empty())
        return;

    ScopedInternal internal;
    Frame& f = r.frames[r.next];
    // The slot's previous overlay must be off the GPU before its allocator is reset.
    if (f.fenceValue && r.fence->GetCompletedValue() < f.fenceValue)
    {
        if (FAILED(r.fence->SetEventOnCompletion(f.fenceValue, r.event)))
            return;
        WaitForSingleObject(r.event, 1000);
    }
    if (!EnsureVertexBuffer(device, f, (uint32_t)rects.size()))
        return;
    memcpy(f.mapped, rects.data(), rects.size() * sizeof(gpuhud::Rect));

    if (FAILED(f.allocator->Reset()))
        return;
    if (FAILED(f.list->Reset(f.allocator.get(), s->pipeline.get())))
        return;

    // The application must leave the back buffer in PRESENT before presenting, so that is where
    // this starts, and where it has to put it back.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = s->buffers[index].get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    f.list->ResourceBarrier(1, &barrier);

    f.list->OMSetRenderTargets(1, &s->rtvs[index], FALSE, nullptr);
    f.list->SetGraphicsRootSignature(r.rootSignature.get());
    const float push[4] = {1.0f / (float)s->width, 1.0f / (float)s->height, 0.0f, 0.0f};
    f.list->SetGraphicsRoot32BitConstants(0, 4, push, 0);
    D3D12_VIEWPORT viewport{0.0f, 0.0f, (float)s->width, (float)s->height, 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, (LONG)s->width, (LONG)s->height};
    f.list->RSSetViewports(1, &viewport);
    f.list->RSSetScissorRects(1, &scissor);
    f.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = f.vertices->GetGPUVirtualAddress();
    vbv.SizeInBytes = (UINT)(rects.size() * sizeof(gpuhud::Rect));
    vbv.StrideInBytes = sizeof(gpuhud::Rect);
    f.list->IASetVertexBuffers(0, 1, &vbv);
    f.list->DrawInstanced(4, (UINT)rects.size(), 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    f.list->ResourceBarrier(1, &barrier);
    if (FAILED(f.list->Close()))
        return;

    ID3D12CommandList* lists[] = {f.list.get()};
    queue->ExecuteCommandLists(1, lists);
    f.fenceValue = ++r.nextFenceValue;
    queue->Signal(r.fence.get(), f.fenceValue);
    r.next = (r.next + 1) % r.frames.size();
}

// -----------------------------------------------------------------------------------------------
// Teardown

void Hud::OnResizeBuffers(IDXGISwapChain* swapChain)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _swapChains.find(swapChain);
    if (it == _swapChains.end())
        return;
    // Anything still drawing into these back buffers has to finish before the swap chain destroys
    // them; the application's own guarantee covers its work, not the library's.
    ScopedInternal internal;
    for (auto& device : _devices)
    {
        DeviceResources& r = device.second;
        if (!r.fence)
            continue;
        for (auto& f : r.frames)
        {
            if (!f.fenceValue || r.fence->GetCompletedValue() >= f.fenceValue)
                continue;
            if (SUCCEEDED(r.fence->SetEventOnCompletion(f.fenceValue, r.event)))
                WaitForSingleObject(r.event, 1000);
        }
    }
    _swapChains.erase(it);
}

void Hud::OnReleaseSwapChain(IDXGISwapChain* swapChain) { OnResizeBuffers(swapChain); }

} // namespace dxinsp
