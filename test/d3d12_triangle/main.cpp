// Minimal Direct3D 12 test application, the counterpart of test/triangle: two rotating textured
// cubes with a depth buffer, re-recording the command list every frame. Exercises what the D3D12
// capture library has to see: a device and a swap chain, committed resources in default and
// upload heaps, descriptor heaps and views, a root signature with a descriptor table, root
// constants and a static sampler, graphics and compute pipelines, per-frame command recording,
// barriers and presents.
//
// Usage: dxinsp_triangle [--frames N] [--width W] [--height H] [--msaa] [--bundle] [--indirect]
//                        [--render-pass] [--compute] [--offscreen] [--leak] [--debug-layer]
//
// The window is resizable: the swap chain's buffers, the depth buffer and the multisampled target
// are recreated when the window size changes, which exercises the inspector's handling of object
// destruction and back buffer replacement.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                              \
    do {                                                                                      \
        HRESULT hr_ = (x);                                                                    \
        if (FAILED(hr_)) {                                                                    \
            fprintf(stderr, "%s failed: 0x%08lx (%s:%d)\n", #x, (unsigned long)hr_, __FILE__, __LINE__); \
            exit(1);                                                                          \
        }                                                                                     \
    } while (0)

namespace {

struct Vertex {
    float pos[3];
    float color[3];
    float uv[2];
};

// Column-major, like cube.hlsl's default float4x4 packing: mul(M, v) is M * v.
struct Mat4 {
    float m[16];
};

Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

Mat4 Perspective(float fovy, float aspect, float zn, float zf) {
    float f = 1.0f / tanf(fovy / 2);
    Mat4 r{};
    r.m[0] = f / aspect;
    r.m[5] = f;   // D3D clip space: y up, z in [0, 1]
    r.m[10] = zf / (zn - zf);
    r.m[11] = -1;
    r.m[14] = (zn * zf) / (zn - zf);
    return r;
}

Mat4 Translate(float x, float y, float z) {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

Mat4 RotateY(float a) {
    Mat4 r{};
    r.m[0] = cosf(a); r.m[2] = -sinf(a); r.m[5] = 1; r.m[8] = sinf(a); r.m[10] = cosf(a); r.m[15] = 1;
    return r;
}

Mat4 RotateX(float a) {
    Mat4 r{};
    r.m[0] = 1; r.m[5] = cosf(a); r.m[6] = sinf(a); r.m[9] = -sinf(a); r.m[10] = cosf(a); r.m[15] = 1;
    return r;
}

std::vector<char> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path.c_str());
        exit(1);
    }
    std::vector<char> data((size_t)f.tellg());
    f.seekg(0);
    f.read(data.data(), (std::streamsize)data.size());
    return data;
}

std::string ExeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf);
    return s.substr(0, s.find_last_of("\\/") + 1);
}

// cube.hlsl's cbuffer Cube. Each frame slot's copy sits at a 256-byte offset, the alignment a CBV
// needs.
struct CubeConstants {
    Mat4 viewProj;
    Mat4 model;
};
constexpr uint32_t kConstantSlot = 256;

// Root parameter 1 of both root signatures: two 32-bit root constants.
struct RootConstants {
    float time;
    uint32_t value;   // cube.hlsl: flags; wave.hlsl: count
};

constexpr uint32_t kTextureSize = 8;
constexpr uint32_t kTextureMips = 4;   // 8x8 -> 1x1 (the inspector reads every mip back)
constexpr uint32_t kWaveCount = 1024;
constexpr uint32_t kFrameCount = 3;    // back buffers, and frames in flight
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

// The shader-visible heap: {CBV, SRV} per frame slot, so root parameter 0's table is two
// consecutive slots (b0 then t0), followed by the wave buffer's UAV.
constexpr uint32_t kHeapUav = 2 * kFrameCount;
constexpr uint32_t kHeapSize = kHeapUav + 1;

struct App {
    uint32_t width = 640, height = 480;
    uint32_t maxFrames = 0;   // 0: until the window is closed
    // --msaa: the cubes render into a 4x multisampled target and depth buffer, resolved into the
    // back buffer with ResolveSubresource (the capture resolves the targets it reads back).
    bool msaa = false;
    // --bundle: the draw and its bindings are recorded once per frame slot in a bundle and run
    // with ExecuteBundle; the bundle binds the root signature and the table itself, so the
    // capture's snapshot of a table set inside a bundle has contents. Its root constants are
    // fixed (time 0, flags 1), so the tint stands still and the channels are swapped.
    bool bundle = false;
    // --indirect: the draw's arguments come from a buffer through ExecuteIndirect.
    bool indirect = false;
    // --render-pass: BeginRenderPass / EndRenderPass with CLEAR and PRESERVE accesses instead of
    // OMSetRenderTargets and the clears (ID3D12GraphicsCommandList4).
    bool renderPass = false;
    // --compute: every frame dispatches wave.hlsl into a UAV before the draw. Nothing reads it.
    bool compute = false;
    // --offscreen: no swap chain, no present; renders into its own targets, as Chrome's Dawn
    // WebGPU device does. The inspector's frame boundary falls back to the per-frame submit.
    bool offscreen = false;
    bool leak = false;         // one buffer is never released (the inspector's leak report)
    bool debugLayer = false;   // the application enables the D3D12 debug layer itself
    bool resized = false;      // the swap chain must be resized before the next frame

    HWND hwnd = nullptr;
    bool quit = false;

    // The device before everything made from it: members release in reverse order, and the
    // device's last Release is what makes the inspector report leaks.
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12CommandAllocator> allocators[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12GraphicsCommandList4> list4;   // --render-pass
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    uint64_t fenceValues[kFrameCount]{};
    uint64_t nextFenceValue = 1;
    uint32_t frameIndex = 0;
    uint64_t frameCount = 0;

    ComPtr<ID3D12DescriptorHeap> rtvHeap;   // one RTV per back buffer, then the MSAA target's
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    ComPtr<ID3D12DescriptorHeap> srvHeap;   // shader-visible CBV/SRV/UAV heap (kHeapSize)
    uint32_t rtvSize = 0, srvSize = 0;
    ComPtr<ID3D12Resource> backBuffers[kFrameCount];
    ComPtr<ID3D12Resource> depthBuffer;
    ComPtr<ID3D12Resource> msaaTarget;

    ComPtr<ID3D12Resource> vertexBuffer, indexBuffer, constantBuffer, texture, waveBuffer, indirectArgs;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW indexBufferView{};
    uint8_t* constantMapped = nullptr;
    std::vector<ComPtr<ID3D12Resource>> uploads;   // staging buffers, alive until their copies ran

    ComPtr<ID3D12RootSignature> rootSignature, computeRootSignature;
    ComPtr<ID3D12PipelineState> pipeline, computePipeline;
    ComPtr<ID3D12CommandSignature> commandSignature;
    ComPtr<ID3D12CommandAllocator> bundleAllocator;
    ComPtr<ID3D12GraphicsCommandList> bundles[kFrameCount];   // --bundle: one per frame slot

    // --------------------------------------------------------------------------------- window
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        App* app = (App*)GetWindowLongPtrA(h, GWLP_USERDATA);
        if (msg == WM_CLOSE || msg == WM_DESTROY) {
            if (app) app->quit = true;
            return 0;
        }
        if (msg == WM_KEYDOWN && w == VK_ESCAPE && app) app->quit = true;
        if (msg == WM_SIZE && app) app->resized = true;
        return DefWindowProcA(h, msg, w, l);
    }

    void CreateWindowNative() {
        WNDCLASSA wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "dxinsp_triangle";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassA(&wc);
        RECT r{0, 0, (LONG)width, (LONG)height};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowA(wc.lpszClassName, "GPU Inspector test: D3D12 cube", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                             wc.hInstance, nullptr);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)this);
        resized = false;   // the WM_SIZE of creation
    }

    void PumpEvents() {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    // --------------------------------------------------------------------------------- helpers
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle(uint32_t index) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)index * rtvSize;
        return h;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE SrvCpuHandle(uint32_t index) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)index * srvSize;
        return h;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle(uint32_t index) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)index * srvSize;
        return h;
    }

    static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        return b;
    }

    ComPtr<ID3D12Resource> CreateBuffer(D3D12_HEAP_TYPE heap, uint64_t size, D3D12_RESOURCE_STATES state,
                                        D3D12_RESOURCE_FLAGS flags, const wchar_t* name) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ComPtr<ID3D12Resource> r;
        CHECK(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)));
        r->SetName(name);
        return r;
    }

    // A default-heap buffer filled through an upload buffer: the copy and the transition to its
    // final state are recorded on the list between BeginUpload and EndUpload. A buffer is always
    // created in COMMON (the runtime ignores any other initial state) and the copy promotes it to
    // COPY_DEST, which is what the barrier transitions from.
    ComPtr<ID3D12Resource> CreateBufferWithData(const void* data, uint64_t size, D3D12_RESOURCE_STATES state,
                                                const wchar_t* name) {
        ComPtr<ID3D12Resource> buffer = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_COMMON,
                                                     D3D12_RESOURCE_FLAG_NONE, name);
        ComPtr<ID3D12Resource> staging = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      D3D12_RESOURCE_FLAG_NONE, L"Upload staging");
        void* mapped = nullptr;
        CHECK(staging->Map(0, nullptr, &mapped));
        memcpy(mapped, data, (size_t)size);
        staging->Unmap(0, nullptr);
        list->CopyBufferRegion(buffer.Get(), 0, staging.Get(), 0, size);
        D3D12_RESOURCE_BARRIER b = Transition(buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, state);
        list->ResourceBarrier(1, &b);
        uploads.push_back(staging);
        return buffer;
    }

    void BeginUpload() {
        CHECK(allocators[0]->Reset());
        CHECK(list->Reset(allocators[0].Get(), nullptr));
    }

    void EndUpload() {
        CHECK(list->Close());
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitForGpu();
        uploads.clear();
    }

    void WaitForGpu() {
        CHECK(queue->Signal(fence.Get(), nextFenceValue));
        CHECK(fence->SetEventOnCompletion(nextFenceValue, fenceEvent));
        WaitForSingleObject(fenceEvent, INFINITE);
        ++nextFenceValue;
    }

    // Blocks until the frame that last used this frame slot has finished on the GPU.
    void WaitForFrame(uint32_t slot) {
        if (fence->GetCompletedValue() < fenceValues[slot]) {
            CHECK(fence->SetEventOnCompletion(fenceValues[slot], fenceEvent));
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    }

    // --------------------------------------------------------------------------------- setup
    void InitDevice() {
        UINT factoryFlags = 0;
        if (debugLayer) {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            } else {
                fprintf(stderr, "--debug-layer: the D3D12 debug layer is not installed (Windows' Graphics Tools)\n");
            }
        }
        CHECK(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)));

        // The first hardware adapter that makes a feature level 11.0 device, fastest first where
        // the factory can order them.
        ComPtr<IDXGIFactory6> factory6;
        factory.As(&factory6);
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            HRESULT hr = factory6 ? factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate))
                                  : factory->EnumAdapters1(i, &candidate);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            CHECK(hr);
            DXGI_ADAPTER_DESC1 desc;
            candidate->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
                adapter = candidate;
                break;
            }
        }
        if (!device) {
            fprintf(stderr, "no Direct3D 12 adapter\n");
            exit(1);
        }
        device->SetName(L"Device");

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
        queue->SetName(L"Direct queue");

        for (uint32_t i = 0; i < kFrameCount; ++i) {
            CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])));
            wchar_t name[32];
            swprintf(name, 32, L"Frame allocator %u", i);
            allocators[i]->SetName(name);
        }
        // Created open, and closed at once: every frame reopens it with Reset.
        CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&list)));
        CHECK(list->Close());
        list->SetName(L"Frame command list");
        if (renderPass && FAILED(list.As(&list4))) {
            fprintf(stderr, "--render-pass: ID3D12GraphicsCommandList4 is not available (Windows 10 1809 or newer)\n");
            exit(1);
        }

        CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        fence->SetName(L"Frame fence");
        fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kFrameCount + 1;
        CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)));
        rtvHeap->SetName(L"RTV heap");
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap)));
        dsvHeap->SetName(L"DSV heap");
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kHeapSize;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap)));
        srvHeap->SetName(L"CBV/SRV/UAV heap");
        rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void CreateSwapChain() {
        // --offscreen renders into its own textures and never presents, the way Chrome's Dawn
        // WebGPU device on D3D12 renders into textures the compositor presents rather than
        // presenting itself. There is no swap chain and no IDXGISwapChain::Present, so the
        // inspector's frame boundary falls back to the per-frame ExecuteCommandLists.
        if (offscreen) {
            frameIndex = 0;
            CreateSizedResources();
            return;
        }
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = width;
        sd.Height = height;
        sd.Format = kColorFormat;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kFrameCount;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> sc1;
        CHECK(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1));
        CHECK(sc1.As(&swapChain));
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        const char name[] = "Swap chain";   // DXGI objects have no SetName
        swapChain->SetPrivateData(WKPDID_D3DDebugObjectName, sizeof(name) - 1, name);
        frameIndex = swapChain->GetCurrentBackBufferIndex();
        CreateSizedResources();
    }

    // The back buffers' RTVs, the depth buffer and (--msaa) the multisampled colour target, for
    // the current window size.
    void CreateSizedResources() {
        for (uint32_t i = 0; i < kFrameCount; ++i) {
            if (offscreen) {
                // The counterpart of a swap-chain back buffer, held in RENDER_TARGET the whole
                // time since it is never presented.
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC rd{};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                rd.Width = width;
                rd.Height = height;
                rd.DepthOrArraySize = 1;
                rd.MipLevels = 1;
                rd.Format = kColorFormat;
                rd.SampleDesc.Count = 1;
                rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                D3D12_CLEAR_VALUE clear{};
                clear.Format = kColorFormat;
                memcpy(clear.Color, kClearColor, sizeof(kClearColor));
                CHECK(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                      IID_PPV_ARGS(&backBuffers[i])));
            } else {
                CHECK(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i])));
            }
            wchar_t name[32];
            swprintf(name, 32, L"Back buffer %u", i);
            backBuffers[i]->SetName(name);
            device->CreateRenderTargetView(backBuffers[i].Get(), nullptr, RtvHandle(i));
        }

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width;
        rd.Height = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = kDepthFormat;
        rd.SampleDesc.Count = msaa ? 4 : 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = kDepthFormat;
        clear.DepthStencil.Depth = 1.0f;
        CHECK(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                              IID_PPV_ARGS(&depthBuffer)));
        depthBuffer->SetName(L"Depth buffer");
        device->CreateDepthStencilView(depthBuffer.Get(), nullptr, dsvHeap->GetCPUDescriptorHandleForHeapStart());

        if (msaa) {
            rd.Format = kColorFormat;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            clear.Format = kColorFormat;
            memcpy(clear.Color, kClearColor, sizeof(kClearColor));
            CHECK(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                  IID_PPV_ARGS(&msaaTarget)));
            msaaTarget->SetName(L"MSAA color target");
            device->CreateRenderTargetView(msaaTarget.Get(), nullptr, RtvHandle(kFrameCount));
        }
    }

    void ReleaseSizedResources() {
        for (auto& b : backBuffers) b.Reset();
        depthBuffer.Reset();
        msaaTarget.Reset();
    }

    // Recreates the swap chain's buffers for the window's client area. Returns false when there
    // is nothing to draw into (minimized).
    bool Resize() {
        resized = false;
        if (offscreen) return true;   // no swap chain to resize; the offscreen targets keep their size
        RECT r;
        GetClientRect(hwnd, &r);
        uint32_t w = (uint32_t)(r.right - r.left), h = (uint32_t)(r.bottom - r.top);
        if (w == 0 || h == 0) return false;
        if (w == width && h == height) return true;
        // ResizeBuffers refuses while anything still references the buffers.
        WaitForGpu();
        ReleaseSizedResources();
        width = w;
        height = h;
        CHECK(swapChain->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_UNKNOWN, 0));
        CreateSizedResources();
        frameIndex = swapChain->GetCurrentBackBufferIndex();
        return true;
    }

    void CreatePipelines() {
        // Graphics root signature: [0] a table {CBV b0, SRV t0}, [1] root constants b1, and a
        // static point sampler s0.
        D3D12_DESCRIPTOR_RANGE1 ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        ranges[0].NumDescriptors = 1;
        ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 1;
        ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER1 params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2;
        params[0].DescriptorTable.pDescriptorRanges = ranges;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 1;
        params[1].Constants.Num32BitValues = sizeof(RootConstants) / 4;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
        rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rs.Desc_1_1.NumParameters = 2;
        rs.Desc_1_1.pParameters = params;
        rs.Desc_1_1.NumStaticSamplers = 1;
        rs.Desc_1_1.pStaticSamplers = &sampler;
        rs.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        rootSignature = MakeRootSignature(rs, L"Cube root signature");

        // Compute root signature: [0] a table {UAV u0}, [1] root constants b0.
        D3D12_DESCRIPTOR_RANGE1 uavRange{};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &uavRange;
        params[1].Constants.ShaderRegister = 0;
        rs.Desc_1_1.NumStaticSamplers = 0;
        rs.Desc_1_1.pStaticSamplers = nullptr;
        rs.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        computeRootSignature = MakeRootSignature(rs, L"Wave root signature");

        std::vector<char> vs = ReadFile(ExeDir() + "cube_vs.cso");
        std::vector<char> ps = ReadFile(ExeDir() + "cube_ps.cso");
        std::vector<char> cs = ReadFile(ExeDir() + "wave_cs.cso");

        D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, pos), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rootSignature.Get();
        pd.VS = {vs.data(), vs.size()};
        pd.PS = {ps.data(), ps.size()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        pd.RasterizerState.FrontCounterClockwise = TRUE;   // the cube's faces wind CCW seen from outside
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pd.InputLayout = {layout, 3};
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = kColorFormat;
        pd.DSVFormat = kDepthFormat;
        pd.SampleDesc.Count = msaa ? 4 : 1;
        CHECK(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipeline)));
        pipeline->SetName(L"Cube pipeline");

        D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
        cd.pRootSignature = computeRootSignature.Get();
        cd.CS = {cs.data(), cs.size()};
        CHECK(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&computePipeline)));
        computePipeline->SetName(L"Wave pipeline");

        // --indirect: one DrawIndexedInstanced per argument record, no root arguments in it.
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC csd{};
        csd.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        csd.NumArgumentDescs = 1;
        csd.pArgumentDescs = &arg;
        CHECK(device->CreateCommandSignature(&csd, nullptr, IID_PPV_ARGS(&commandSignature)));
        commandSignature->SetName(L"Draw indexed signature");
    }

    ComPtr<ID3D12RootSignature> MakeRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc, const wchar_t* name) {
        ComPtr<ID3DBlob> blob, error;
        HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
        if (FAILED(hr)) {
            fprintf(stderr, "root signature: %s\n", error ? (const char*)error->GetBufferPointer() : "?");
            exit(1);
        }
        ComPtr<ID3D12RootSignature> rs;
        CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
        rs->SetName(name);
        return rs;
    }

    void CreateResources() {
        // Cube geometry: six quads, each with its own colour, wound counter-clockwise seen from
        // outside.
        const float p = 0.5f;
        Vertex verts[24];
        uint16_t indices[36];
        const float faces[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const float colors[6][3] = {{1, 0.3f, 0.3f}, {0.3f, 1, 0.3f}, {0.3f, 0.3f, 1}, {1, 1, 0.3f}, {1, 0.3f, 1}, {0.3f, 1, 1}};
        int v = 0, ix = 0;
        for (int f = 0; f < 6; ++f) {
            const float* n = faces[f];
            float u[3] = {n[1], n[2], n[0]};
            float w[3] = {n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0]};
            for (int c = 0; c < 4; ++c) {
                float su = (c == 1 || c == 2) ? 1.f : -1.f;
                float sv = (c >= 2) ? 1.f : -1.f;
                for (int k = 0; k < 3; ++k) verts[v].pos[k] = p * (n[k] + su * u[k] + sv * w[k]);
                memcpy(verts[v].color, colors[f], sizeof(verts[v].color));
                verts[v].uv[0] = su * 0.5f + 0.5f;
                verts[v].uv[1] = sv * 0.5f + 0.5f;
                ++v;
            }
            uint16_t b = (uint16_t)(f * 4);
            uint16_t quad[6] = {b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3)};
            for (int k = 0; k < 6; ++k) indices[ix++] = quad[k];
        }
        D3D12_DRAW_INDEXED_ARGUMENTS drawArgs{36, 2, 0, 0, 0};

        BeginUpload();
        vertexBuffer = CreateBufferWithData(verts, sizeof(verts), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, L"Cube vertices");
        indexBuffer = CreateBufferWithData(indices, sizeof(indices), D3D12_RESOURCE_STATE_INDEX_BUFFER, L"Cube indices");
        indirectArgs = CreateBufferWithData(&drawArgs, sizeof(drawArgs), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, L"Draw arguments");
        CreateTexture();
        EndUpload();
        vertexBufferView = {vertexBuffer->GetGPUVirtualAddress(), sizeof(verts), sizeof(Vertex)};
        indexBufferView = {indexBuffer->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R16_UINT};

        // The constant buffer stays mapped: an upload heap is written by the CPU every frame,
        // one slot per frame in flight.
        constantBuffer = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, kConstantSlot * kFrameCount, D3D12_RESOURCE_STATE_GENERIC_READ,
                                      D3D12_RESOURCE_FLAG_NONE, L"Cube constants");
        CHECK(constantBuffer->Map(0, nullptr, (void**)&constantMapped));

        // Promoted to UNORDERED_ACCESS by the dispatch, back to COMMON when the frame's lists finish.
        waveBuffer = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, kWaveCount * sizeof(float), D3D12_RESOURCE_STATE_COMMON,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Wave buffer");

        if (leak) CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, 4096, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, L"Leaked buffer").Detach();

        // Views into the shader-visible heap.
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = kColorFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = kTextureMips;
        for (uint32_t i = 0; i < kFrameCount; ++i) {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
            cbv.BufferLocation = constantBuffer->GetGPUVirtualAddress() + i * kConstantSlot;
            cbv.SizeInBytes = kConstantSlot;
            device->CreateConstantBufferView(&cbv, SrvCpuHandle(2 * i));
            device->CreateShaderResourceView(texture.Get(), &srv, SrvCpuHandle(2 * i + 1));
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = kWaveCount;
        uav.Buffer.StructureByteStride = sizeof(float);
        device->CreateUnorderedAccessView(waveBuffer.Get(), nullptr, &uav, SrvCpuHandle(kHeapUav));
    }

    // An 8x8 RGBA8 checkerboard with a mip chain, every level box-filtered from the one above on
    // the CPU and uploaded through one staging buffer with a CopyTextureRegion per level.
    void CreateTexture() {
        std::vector<uint8_t> levels[kTextureMips];
        levels[0].resize(kTextureSize * kTextureSize * 4);
        for (uint32_t y = 0; y < kTextureSize; ++y)
            for (uint32_t x = 0; x < kTextureSize; ++x) {
                uint8_t c = ((x + y) & 1) ? 255 : 90;
                uint8_t* px = &levels[0][(y * kTextureSize + x) * 4];
                px[0] = px[1] = px[2] = c;
                px[3] = 255;
            }
        for (uint32_t level = 1; level < kTextureMips; ++level) {
            uint32_t src = kTextureSize >> (level - 1), dst = kTextureSize >> level;
            levels[level].resize(dst * dst * 4);
            for (uint32_t y = 0; y < dst; ++y)
                for (uint32_t x = 0; x < dst; ++x)
                    for (uint32_t k = 0; k < 4; ++k) {
                        uint32_t sum = 0;
                        for (uint32_t dy = 0; dy < 2; ++dy)
                            for (uint32_t dx = 0; dx < 2; ++dx) sum += levels[level - 1][((2 * y + dy) * src + 2 * x + dx) * 4 + k];
                        levels[level][(y * dst + x) * 4 + k] = (uint8_t)(sum / 4);
                    }
        }

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kTextureSize;
        td.Height = kTextureSize;
        td.DepthOrArraySize = 1;
        td.MipLevels = kTextureMips;
        td.Format = kColorFormat;
        td.SampleDesc.Count = 1;
        CHECK(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&texture)));
        texture->SetName(L"Checker texture");

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[kTextureMips];
        UINT rows[kTextureMips];
        UINT64 rowSizes[kTextureMips], total;
        device->GetCopyableFootprints(&td, 0, kTextureMips, 0, layouts, rows, rowSizes, &total);
        ComPtr<ID3D12Resource> staging = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      D3D12_RESOURCE_FLAG_NONE, L"Texture staging");
        uint8_t* mapped = nullptr;
        CHECK(staging->Map(0, nullptr, (void**)&mapped));
        for (uint32_t level = 0; level < kTextureMips; ++level)
            for (UINT y = 0; y < rows[level]; ++y)
                memcpy(mapped + layouts[level].Offset + y * layouts[level].Footprint.RowPitch,
                       &levels[level][y * rowSizes[level]], (size_t)rowSizes[level]);
        staging->Unmap(0, nullptr);
        for (uint32_t level = 0; level < kTextureMips; ++level) {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = texture.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = level;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = staging.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = layouts[level];
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        // The table that binds it is visible to every stage, so both shader-resource states.
        D3D12_RESOURCE_BARRIER b = Transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &b);
        uploads.push_back(staging);
    }

    // --bundle: the draw with everything it binds, once per frame slot (each slot has its own
    // constant buffer view). A bundle that sets a root signature must set its root arguments too.
    void RecordBundles() {
        CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundleAllocator)));
        bundleAllocator->SetName(L"Bundle allocator");
        for (uint32_t i = 0; i < kFrameCount; ++i) {
            CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundleAllocator.Get(), pipeline.Get(),
                                            IID_PPV_ARGS(&bundles[i])));
            wchar_t name[32];
            swprintf(name, 32, L"Cube bundle %u", i);
            bundles[i]->SetName(name);
            ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
            bundles[i]->SetDescriptorHeaps(1, heaps);
            bundles[i]->SetGraphicsRootSignature(rootSignature.Get());
            bundles[i]->SetGraphicsRootDescriptorTable(0, SrvGpuHandle(2 * i));
            RootConstants constants{0.0f, 1u};
            bundles[i]->SetGraphicsRoot32BitConstants(1, sizeof(constants) / 4, &constants, 0);
            bundles[i]->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            bundles[i]->IASetVertexBuffers(0, 1, &vertexBufferView);
            bundles[i]->IASetIndexBuffer(&indexBufferView);
            bundles[i]->DrawIndexedInstanced(36, 2, 0, 0, 0);
            CHECK(bundles[i]->Close());
        }
    }

    // --------------------------------------------------------------------------------- frame
    static constexpr float kClearColor[4] = {0.1f, 0.1f, 0.15f, 1.0f};

    bool DrawFrame(float t) {
        if (resized && !Resize()) return false;
        WaitForFrame(frameIndex);

        Mat4 proj = Perspective(1.0f, (float)width / (float)height, 0.1f, 20.0f);
        Mat4 view = Translate(0, 0, -4.0f);
        CubeConstants constants{Mul(proj, view), Mul(RotateY(t), RotateX(t * 0.7f))};
        memcpy(constantMapped + frameIndex * kConstantSlot, &constants, sizeof(constants));

        CHECK(allocators[frameIndex]->Reset());
        CHECK(list->Reset(allocators[frameIndex].Get(), pipeline.Get()));
        ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
        list->SetDescriptorHeaps(1, heaps);

        // The wave dispatch, before the render targets are touched: a compute pass of its own.
        if (compute) {
            list->SetComputeRootSignature(computeRootSignature.Get());
            list->SetPipelineState(computePipeline.Get());
            list->SetComputeRootDescriptorTable(0, SrvGpuHandle(kHeapUav));
            RootConstants params{t, kWaveCount};
            list->SetComputeRoot32BitConstants(1, sizeof(params) / 4, &params, 0);
            list->Dispatch(kWaveCount / 64, 1, 1);
            D3D12_RESOURCE_BARRIER uav{};
            uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav.UAV.pResource = waveBuffer.Get();
            list->ResourceBarrier(1, &uav);
            list->SetPipelineState(pipeline.Get());
        }

        ID3D12Resource* backBuffer = backBuffers[frameIndex].Get();
        // The state the target rests in between frames: PRESENT for a swap-chain buffer, or
        // RENDER_TARGET for an offscreen one that is never presented.
        const D3D12_RESOURCE_STATES idleState = offscreen ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_PRESENT;
        // With MSAA the back buffer is only ever the resolve destination.
        D3D12_RESOURCE_STATES backBufferState = msaa ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (idleState != backBufferState) {
            D3D12_RESOURCE_BARRIER toTarget = Transition(backBuffer, idleState, backBufferState);
            list->ResourceBarrier(1, &toTarget);
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvHandle(msaa ? kFrameCount : frameIndex);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        if (renderPass) {
            D3D12_RENDER_PASS_RENDER_TARGET_DESC rt{};
            rt.cpuDescriptor = rtv;
            rt.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
            rt.BeginningAccess.Clear.ClearValue.Format = kColorFormat;
            memcpy(rt.BeginningAccess.Clear.ClearValue.Color, kClearColor, sizeof(kClearColor));
            rt.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
            D3D12_RENDER_PASS_DEPTH_STENCIL_DESC ds{};
            ds.cpuDescriptor = dsv;
            ds.DepthBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
            ds.DepthBeginningAccess.Clear.ClearValue.Format = kDepthFormat;
            ds.DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth = 1.0f;
            ds.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
            ds.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
            ds.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
            list4->BeginRenderPass(1, &rt, &ds, D3D12_RENDER_PASS_FLAG_NONE);
        } else {
            list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            list->ClearRenderTargetView(rtv, kClearColor, 0, nullptr);
            list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        }

        D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
        D3D12_RECT scissor{0, 0, (LONG)width, (LONG)height};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetGraphicsRootSignature(rootSignature.Get());
        list->SetGraphicsRootDescriptorTable(0, SrvGpuHandle(2 * frameIndex));
        RootConstants frame{t, 0u};
        list->SetGraphicsRoot32BitConstants(1, sizeof(frame) / 4, &frame, 0);
        if (bundle) {
            list->ExecuteBundle(bundles[frameIndex].Get());
        } else {
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &vertexBufferView);
            list->IASetIndexBuffer(&indexBufferView);
            if (indirect)
                list->ExecuteIndirect(commandSignature.Get(), 1, indirectArgs.Get(), 0, nullptr, 0);
            else
                list->DrawIndexedInstanced(36, 2, 0, 0, 0);
        }
        if (renderPass) list4->EndRenderPass();

        if (msaa) {
            D3D12_RESOURCE_BARRIER toResolve = Transition(msaaTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            list->ResourceBarrier(1, &toResolve);
            list->ResolveSubresource(backBuffer, 0, msaaTarget.Get(), 0, kColorFormat);
            D3D12_RESOURCE_BARRIER after[2] = {
                Transition(msaaTarget.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                Transition(backBuffer, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_PRESENT),
            };
            list->ResourceBarrier(2, after);
        } else if (idleState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER toPresent = Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, idleState);
            list->ResourceBarrier(1, &toPresent);
        }

        CHECK(list->Close());
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (!offscreen) CHECK(swapChain->Present(1, 0));
        CHECK(queue->Signal(fence.Get(), nextFenceValue));
        fenceValues[frameIndex] = nextFenceValue++;
        frameIndex = offscreen ? (frameIndex + 1) % kFrameCount : swapChain->GetCurrentBackBufferIndex();
        ++frameCount;
        return true;
    }

    void Cleanup() {
        WaitForGpu();
        constantBuffer->Unmap(0, nullptr);
        CloseHandle(fenceEvent);
        // Everything else is released by the members' destructors, the device last.
    }

    int Run() {
        CreateWindowNative();
        InitDevice();
        CreateSwapChain();
        CreatePipelines();
        CreateResources();
        if (bundle) RecordBundles();
        auto start = std::chrono::steady_clock::now();
        auto nextFrame = start;
        while (!quit && (maxFrames == 0 || frameCount < maxFrames)) {
            PumpEvents();
            if (quit) break;
            float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
            if (!DrawFrame(t)) Sleep(16);
            // A swap chain paces the loop to the display; an offscreen renderer has nothing to
            // wait on and would spin a core at thousands of fps, so it is paced to ~60 the way a
            // real WebGPU application is driven by requestAnimationFrame.
            if (offscreen) {
                nextFrame += std::chrono::microseconds(16667);
                auto now = std::chrono::steady_clock::now();
                if (nextFrame > now) std::this_thread::sleep_for(nextFrame - now);
                else nextFrame = now;
            }
        }
        Cleanup();
        return 0;
    }
};

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    App app;
    int argc = __argc;
    char** argv = __argv;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) app.maxFrames = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) app.width = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) app.height = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--msaa")) app.msaa = true;
        else if (!strcmp(argv[i], "--bundle")) app.bundle = true;
        else if (!strcmp(argv[i], "--indirect")) app.indirect = true;
        else if (!strcmp(argv[i], "--render-pass")) app.renderPass = true;
        else if (!strcmp(argv[i], "--compute")) app.compute = true;
        else if (!strcmp(argv[i], "--offscreen")) app.offscreen = true;
        else if (!strcmp(argv[i], "--leak")) app.leak = true;
        else if (!strcmp(argv[i], "--debug-layer")) app.debugLayer = true;
        else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 1;
        }
    }
    return app.Run();
}
