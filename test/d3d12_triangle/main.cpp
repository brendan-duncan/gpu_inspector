// Minimal Direct3D 12 test application, the counterpart of test/triangle: two rotating textured
// cubes with a depth buffer, re-recording the command list every frame. Exercises what the D3D12
// capture library has to see: a device and a swap chain, committed resources in default and
// upload heaps, descriptor heaps and views, a root signature with a descriptor table, root
// constants and a static sampler, graphics and compute pipelines, per-frame command recording,
// barriers and presents.
//
// Usage: dxinsp_triangle [--frames N] [--width W] [--height H] [--msaa] [--bundle] [--indirect]
//                        [--render-pass] [--suspend] [--pool] [--compute] [--async-compute] [--offscreen] [--leak]
//                        [--debug-layer] [--stencil]
//                        [--capture-at N] [--churn] [--evict] [--heavy] [--ray-tracing [--rebuild-blas] [--local-root]]
//                        [--bindless]
//
// The window is resizable: the swap chain's buffers, the depth buffer and the multisampled target
// are recreated when the window size changes, which exercises the inspector's handling of object
// destruction and back buffer replacement.

#include <windows.h>
#include <d3d12.h>

#include "gpu_inspector.h"
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

#define CHECK(x)                                                                                         \
    do                                                                                                   \
    {                                                                                                    \
        HRESULT hr_ = (x);                                                                               \
        if (FAILED(hr_))                                                                                 \
        {                                                                                                \
            fprintf(stderr, "%s failed: 0x%08lx (%s:%d)\n", #x, (unsigned long)hr_, __FILE__, __LINE__); \
            exit(1);                                                                                     \
        }                                                                                                \
    } while (0)

namespace
{

struct Vertex
{
    float pos[3];
    float color[3];
    float uv[2];
};

// Column-major, like cube.hlsl's default float4x4 packing: mul(M, v) is M * v.
struct Mat4
{
    float m[16];
};

Mat4 Mul(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
        {
            float s = 0;
            for (int k = 0; k < 4; ++k)
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

Mat4 Perspective(float fovy, float aspect, float zn, float zf)
{
    float f = 1.0f / tanf(fovy / 2);
    Mat4 r{};
    r.m[0] = f / aspect;
    r.m[5] = f;   // D3D clip space: y up, z in [0, 1]
    r.m[10] = zf / (zn - zf);
    r.m[11] = -1;
    r.m[14] = (zn * zf) / (zn - zf);
    return r;
}

Mat4 Translate(float x, float y, float z)
{
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

Mat4 RotateY(float a)
{
    Mat4 r{};
    r.m[0] = cosf(a);
    r.m[2] = -sinf(a);
    r.m[5] = 1;
    r.m[8] = sinf(a);
    r.m[10] = cosf(a);
    r.m[15] = 1;
    return r;
}

Mat4 RotateX(float a)
{
    Mat4 r{};
    r.m[0] = 1;
    r.m[5] = cosf(a);
    r.m[6] = sinf(a);
    r.m[9] = -sinf(a);
    r.m[10] = cosf(a);
    r.m[15] = 1;
    return r;
}

std::vector<char> ReadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
    {
        fprintf(stderr, "cannot open %s\n", path.c_str());
        exit(1);
    }
    std::vector<char> data((size_t)f.tellg());
    f.seekg(0);
    f.read(data.data(), (std::streamsize)data.size());
    return data;
}

std::string ExeDir()
{
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf);
    return s.substr(0, s.find_last_of("\\/") + 1);
}

// cube.hlsl's cbuffer Cube. Each frame slot's copy sits at a 256-byte offset, the alignment a CBV
// needs.
struct CubeConstants
{
    Mat4 viewProj;
    Mat4 model;
};
constexpr uint32_t kConstantSlot = 256;

// Root parameter 1 of both root signatures: two 32-bit root constants.
struct RootConstants
{
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
// consecutive slots (b0 then t0), followed by the wave buffer's UAV, then --ray-tracing's own
// table (the scene's SRV then the traced image's UAV, which must be consecutive).
constexpr uint32_t kHeapUav = 2 * kFrameCount;
constexpr uint32_t kHeapRtScene = kHeapUav + 1;
constexpr uint32_t kHeapRtTarget = kHeapRtScene + 1;
// --local-root: the CBV the tinted hit group's local descriptor table points at. Nothing binds this
// slot through a root table, so only the binding table record names it.
constexpr uint32_t kHeapRtLocal = kHeapRtTarget + 1;
// --bindless: the stripes texture the cube's pixel shader reads through ResourceDescriptorHeap. No
// root table covers this slot, so only the heap's contents at the submission say what it holds.
constexpr uint32_t kHeapBindless = kHeapRtLocal + 1;
constexpr uint32_t kHeapSize = kHeapBindless + 1;

// --ray-tracing: the traced image, and the two instances of the one triangle.
constexpr uint32_t kTraceSize = 256;
constexpr uint32_t kRtInstances = 2;
// The instances are written by the CPU every frame and read by the GPU a build later, so each
// frame in flight gets its own slot. With one shared slot the CPU overwrites the transforms a
// build is still reading -- and the read-back a capture takes of them then holds a later frame's,
// which is exactly the sort of thing a replay turns into a picture that is nearly right.
constexpr uint32_t kRtInstanceSlot = kRtInstances;

struct App
{
    uint32_t width = 640, height = 480;
    uint32_t maxFrames = 0;   // 0: until the window is closed
    uint64_t captureAt = 0;   // --capture-at: ask the inspector for a capture at this frame (gpu_inspector.h)
    bool captureAsked = false;
    // --stall <ms>: sleep this long before each frame, so a vsynced present misses refreshes
    // and the swap chain's statistics have dropped frames to report.
    uint32_t stallMs = 0;
    // --hitch-every N: stall 100 ms inside every Nth frame, in the application's own code, for Capture on hitch.
    uint32_t hitchEvery = 0;
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
    // --suspend: the render pass is suspended at the end of one command list and resumed in the
    // next, which is how an engine that records a frame's passes on worker threads builds them
    // (Unity does). Implies --render-pass; the two lists go in one ExecuteCommandLists, since a
    // suspended pass has to be resumed by the next list the queue runs.
    bool suspend = false;
    // --pool: the frame is recorded into one of kPoolSize lists in turn, each reset as soon as it
    // has run and recorded into again kPoolSize frames later, the way an engine that keeps its
    // lists in a pool does (Unity does). A capture's warm-up frame is shorter than the pool, so the
    // list the captured frame runs was reset before the capture was asked for, and the capture
    // library adopts it at its first command (CaptureManager::Adopt).
    bool pool = false;
    // --compute: every frame dispatches wave.hlsl into a UAV before the draw. Nothing reads it.
    bool compute = false;
    // --async-compute: the wave dispatch goes to a compute queue of its own, in a list of its own,
    // rather than into the frame's list, so it runs beside the render pass instead of before it --
    // what an engine's async compute does, and what gives a capture passes on two queues (the
    // Timeline's lane per queue). Implies --compute. Nothing reads the wave, so neither queue waits
    // on the other within the frame; the direct queue waits for the compute queue's fence only at
    // the frame's end, so the frame fence still says both are done.
    bool asyncCompute = false;
    // --offscreen: no swap chain, no present; renders into its own targets, as Chrome's Dawn
    // WebGPU device does. The inspector's frame boundary falls back to the per-frame submit.
    bool offscreen = false;
    bool leak = false;         // one buffer is never released (the inspector's leak report)
    // --heavy: the cube's pixel shader is heavy.hlsl, whose functions cost known amounts (the
    // Shader Flame Graph's measurements by ablation).
    bool heavy = false;
    // --churn: what a memory capture is for. Every frame makes a small upload buffer and releases
    // the one made two frames before (transient allocations), and every 30th frame makes one that
    // is kept until exit (a slow leak).
    bool churn = false;
    // --evict: a 32 MB default-heap buffer evicted every 120th frame and made resident again 60
    // frames later, so a memory capture and the memory series have residency to mark.
    bool evictMode = false;
    ComPtr<ID3D12Resource> evictable;
    std::vector<ComPtr<ID3D12Resource>> churnRecent, churnKept;
    // --stencil: the depth buffer is D24S8, cleared with the depth and written with 1 wherever the
    // cube draws, so a capture reads a stencil target back beside the depth.
    bool stencil = false;
    // --ray-tracing: each frame rebuilds a top-level acceleration structure over two instances of
    // one triangle's bottom-level structure (built once) and traces a 256x256 UAV with a raygen,
    // a miss and two hit groups, so a capture has a state object with its shader identifiers, both
    // levels of acceleration structure with what they were built from, a shader binding table
    // whose records resolve to exports, and a DispatchRays. The DXR counterpart of
    // test/triangle --ray-tracing.
    //
    // The cubes then take their texture from the traced image rather than the checker, so what the
    // rays wrote is in the render target the capture reads back: a replay that rebuilt the binding
    // table or the instances wrongly shows up as a difference there, and nothing else in the frame
    // would have noticed.
    bool rayTracing = false;
    // --rebuild-blas: the bottom level is rebuilt every frame as well, the way an application with
    // skinned or deformable geometry does. It decides whether a capture holds a build of the bottom
    // level at all, which is what a replay needs to fill it: with the default (built once, before
    // any capture) the replay has the buffer but nothing ever wrote it, and every ray misses.
    bool rebuildBlas = false;
    // --local-root (with --ray-tracing): the tinted hit group gets a local root signature -- a root
    // constant, a root CBV and a table of one CBV -- whose arguments its binding table record holds
    // after the identifier. Those are a GPU address and a GPU descriptor handle of this process,
    // which is what a replay has to translate, and the buffers they name are bound nowhere else, so
    // the capture has to read them back from the record alone.
    bool localRoot = false;
    // --bindless: the cube's root signature lets shaders index the CBV/SRV/UAV heap directly
    // (D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), and its pixel shader
    // (cube_bindless.hlsl, shader model 6.6) multiplies in a texture taken from a heap slot no root
    // table names -- what an engine that is bindless throughout does for every resource.
    bool bindless = false;
    DXGI_FORMAT depthFormat = kDepthFormat;
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
    // --suspend: the list the suspended pass is resumed in, with an allocator per frame slot so it
    // is only reset once the frame it was submitted in has finished.
    ComPtr<ID3D12CommandAllocator> resumeAllocators[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> resumeList;
    ComPtr<ID3D12GraphicsCommandList4> resumeList4;
    // --async-compute: the compute queue, its list with an allocator per frame slot, and its fence.
    ComPtr<ID3D12CommandQueue> computeQueue;
    ComPtr<ID3D12CommandAllocator> computeAllocators[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> computeList;
    ComPtr<ID3D12Fence> computeFence;
    uint64_t computeFenceValue = 0;
    // --pool: the lists and an allocator each; `list` points at the one the frame records into.
    static constexpr uint32_t kPoolSize = 4;
    ComPtr<ID3D12CommandAllocator> poolAllocators[kPoolSize];
    ComPtr<ID3D12GraphicsCommandList> poolLists[kPoolSize];
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
    ComPtr<ID3D12Resource> stripesTexture;   // --bindless
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW indexBufferView{};
    uint8_t* constantMapped = nullptr;
    std::vector<ComPtr<ID3D12Resource>> uploads;   // staging buffers, alive until their copies ran

    ComPtr<ID3D12RootSignature> rootSignature, computeRootSignature;
    ComPtr<ID3D12PipelineState> pipeline, computePipeline;
    ComPtr<ID3D12CommandSignature> commandSignature;
    ComPtr<ID3D12CommandAllocator> bundleAllocator;
    ComPtr<ID3D12GraphicsCommandList> bundles[kFrameCount];   // --bundle: one per frame slot

    // --ray-tracing. A D3D12 acceleration structure is not an object: it is a range inside a UAV
    // buffer, and everything names it by the GPU address a build wrote it to, which is why these
    // are buffers rather than handles.
    struct RayTracing
    {
        ComPtr<ID3D12Device5> device5;
        ComPtr<ID3D12StateObject> stateObject;
        ComPtr<ID3D12RootSignature> rootSignature;
        ComPtr<ID3D12Resource> vertices;        // the triangle the bottom level is built from
        ComPtr<ID3D12Resource> instances;       // D3D12_RAYTRACING_INSTANCE_DESC per instance, mapped
        D3D12_RAYTRACING_INSTANCE_DESC* instancesMapped = nullptr;
        ComPtr<ID3D12Resource> blas, tlas, scratch;
        ComPtr<ID3D12Resource> bindingTable;    // the shader records, mapped and written once
        D3D12_GPU_VIRTUAL_ADDRESS tableAddress = 0;
        ComPtr<ID3D12Resource> target;          // the 256x256 image the rays write
        bool built = false;                     // the bottom level is built once, before the first frame
        // --local-root
        ComPtr<ID3D12RootSignature> localRootSignature;
        ComPtr<ID3D12Resource> localTint;       // the root CBV's buffer
        ComPtr<ID3D12Resource> localTable;      // the buffer the table's CBV views
    } rt;

    // --------------------------------------------------------------------------------- window
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
    {
        App* app = (App*)GetWindowLongPtrA(h, GWLP_USERDATA);
        if (msg == WM_CLOSE || msg == WM_DESTROY)
        {
            if (app)
                app->quit = true;
            return 0;
        }
        if (msg == WM_KEYDOWN && w == VK_ESCAPE && app)
            app->quit = true;
        if (msg == WM_SIZE && app)
            app->resized = true;
        return DefWindowProcA(h, msg, w, l);
    }

    void CreateWindowNative()
    {
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

    void PumpEvents()
    {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    // --------------------------------------------------------------------------------- helpers
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle(uint32_t index)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)index * rtvSize;
        return h;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE SrvCpuHandle(uint32_t index)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)index * srvSize;
        return h;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle(uint32_t index)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)index * srvSize;
        return h;
    }

    static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        return b;
    }

    ComPtr<ID3D12Resource> CreateBuffer(D3D12_HEAP_TYPE heap, uint64_t size, D3D12_RESOURCE_STATES state,
        D3D12_RESOURCE_FLAGS flags, const wchar_t* name)
    {
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
        const wchar_t* name)
    {
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

    void BeginUpload()
    {
        CHECK(allocators[0]->Reset());
        CHECK(list->Reset(allocators[0].Get(), nullptr));
    }

    void EndUpload()
    {
        CHECK(list->Close());
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitForGpu();
        uploads.clear();
    }

    void WaitForGpu()
    {
        CHECK(queue->Signal(fence.Get(), nextFenceValue));
        CHECK(fence->SetEventOnCompletion(nextFenceValue, fenceEvent));
        WaitForSingleObject(fenceEvent, INFINITE);
        ++nextFenceValue;
    }

    // Blocks until the frame that last used this frame slot has finished on the GPU.
    void WaitForFrame(uint32_t slot)
    {
        if (fence->GetCompletedValue() < fenceValues[slot])
        {
            CHECK(fence->SetEventOnCompletion(fenceValues[slot], fenceEvent));
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    }

    // --------------------------------------------------------------------------------- setup
    void InitDevice()
    {
        UINT factoryFlags = 0;
        if (debugLayer)
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            {
                debug->EnableDebugLayer();
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            }
            else
            {
                fprintf(stderr, "--debug-layer: the D3D12 debug layer is not installed (Windows' Graphics Tools)\n");
            }
        }
        CHECK(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)));

        // The first hardware adapter that makes a feature level 11.0 device, fastest first where
        // the factory can order them.
        ComPtr<IDXGIFactory6> factory6;
        factory.As(&factory6);
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> candidate;
            HRESULT hr = factory6 ? factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate))
                                  : factory->EnumAdapters1(i, &candidate);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            CHECK(hr);
            DXGI_ADAPTER_DESC1 desc;
            candidate->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue;
            if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            {
                adapter = candidate;
                break;
            }
        }
        if (!device)
        {
            fprintf(stderr, "no Direct3D 12 adapter\n");
            exit(1);
        }
        device->SetName(L"Device");

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
        queue->SetName(L"Direct queue");

        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])));
            wchar_t name[32];
            swprintf(name, 32, L"Frame allocator %u", i);
            allocators[i]->SetName(name);
        }
        // Created open, and closed at once: every frame reopens it with Reset.
        CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&list)));
        CHECK(list->Close());
        list->SetName(L"Frame command list");
        if (renderPass && FAILED(list.As(&list4)))
        {
            fprintf(stderr, "--render-pass: ID3D12GraphicsCommandList4 is not available (Windows 10 1809 or newer)\n");
            exit(1);
        }
        if (suspend)
        {
            for (uint32_t i = 0; i < kFrameCount; i++)
                CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&resumeAllocators[i])));
            CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, resumeAllocators[0].Get(), nullptr,
                IID_PPV_ARGS(&resumeList)));
            CHECK(resumeList->Close());
            resumeList->SetName(L"Resume command list");
            CHECK(resumeList.As(&resumeList4));
        }
        if (asyncCompute)
        {
            D3D12_COMMAND_QUEUE_DESC cqd{};
            cqd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
            CHECK(device->CreateCommandQueue(&cqd, IID_PPV_ARGS(&computeQueue)));
            computeQueue->SetName(L"Async compute queue");
            for (uint32_t i = 0; i < kFrameCount; i++)
                CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&computeAllocators[i])));
            CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, computeAllocators[0].Get(), nullptr,
                IID_PPV_ARGS(&computeList)));
            CHECK(computeList->Close());
            computeList->SetName(L"Async compute list");
            CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&computeFence)));
        }
        if (pool)
        {
            // Closed until its first turn (the pipeline does not exist yet); from then on each is
            // reset already when its turn comes.
            for (uint32_t i = 0; i < kPoolSize; i++)
            {
                CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&poolAllocators[i])));
                CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, poolAllocators[i].Get(), nullptr,
                    IID_PPV_ARGS(&poolLists[i])));
                CHECK(poolLists[i]->Close());
                wchar_t name[32];
                swprintf(name, 32, L"Pooled command list %u", i);
                poolLists[i]->SetName(name);
            }
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

    void CreateSwapChain()
    {
        // --offscreen renders into its own textures and never presents, the way Chrome's Dawn
        // WebGPU device on D3D12 renders into textures the compositor presents rather than
        // presenting itself. There is no swap chain and no IDXGISwapChain::Present, so the
        // inspector's frame boundary falls back to the per-frame ExecuteCommandLists.
        if (offscreen)
        {
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

    // The back buffers' RTVs, the depth buffer and (--msaa) the multisampled color target, for
    // the current window size.
    void CreateSizedResources()
    {
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (offscreen)
            {
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
            }
            else
            {
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
        rd.Format = depthFormat;
        rd.SampleDesc.Count = msaa ? 4 : 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = depthFormat;
        clear.DepthStencil.Depth = 1.0f;
        CHECK(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
            IID_PPV_ARGS(&depthBuffer)));
        depthBuffer->SetName(L"Depth buffer");
        device->CreateDepthStencilView(depthBuffer.Get(), nullptr, dsvHeap->GetCPUDescriptorHandleForHeapStart());

        if (msaa)
        {
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

    void ReleaseSizedResources()
    {
        for (auto& b : backBuffers)
            b.Reset();
        depthBuffer.Reset();
        msaaTarget.Reset();
    }

    // Recreates the swap chain's buffers for the window's client area. Returns false when there
    // is nothing to draw into (minimized).
    bool Resize()
    {
        resized = false;
        if (offscreen)
            return true;   // no swap chain to resize; the offscreen targets keep their size
        RECT r;
        GetClientRect(hwnd, &r);
        uint32_t w = (uint32_t)(r.right - r.left), h = (uint32_t)(r.bottom - r.top);
        if (w == 0 || h == 0)
            return false;
        if (w == width && h == height)
            return true;
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

    void CreatePipelines()
    {
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
        if (bindless)
            rs.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
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
        std::vector<char> ps = ReadFile(ExeDir() + (bindless ? "cube_bindless_ps.cso" : heavy ? "heavy_ps.cso" : "cube_ps.cso"));
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
        if (stencil)
        {
            // Every fragment that passes writes the stencil reference (1, OMSetStencilRef) into the stencil buffer.
            D3D12_DEPTH_STENCILOP_DESC op{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS};
            pd.DepthStencilState.StencilEnable = TRUE;
            pd.DepthStencilState.StencilReadMask = pd.DepthStencilState.StencilWriteMask = 0xFF;
            pd.DepthStencilState.FrontFace = pd.DepthStencilState.BackFace = op;
        }
        pd.InputLayout = {layout, 3};
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = kColorFormat;
        pd.DSVFormat = depthFormat;
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

    ComPtr<ID3D12RootSignature> MakeRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc, const wchar_t* name)
    {
        ComPtr<ID3DBlob> blob, error;
        HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
        if (FAILED(hr))
        {
            fprintf(stderr, "root signature: %s\n", error ? (const char*)error->GetBufferPointer() : "?");
            exit(1);
        }
        ComPtr<ID3D12RootSignature> rs;
        CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
        rs->SetName(name);
        return rs;
    }

    void CreateResources()
    {
        // Cube geometry: six quads, each with its own color, wound counter-clockwise seen from
        // outside.
        const float p = 0.5f;
        Vertex verts[24];
        uint16_t indices[36];
        const float faces[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const float colors[6][3] = {{1, 0.3f, 0.3f}, {0.3f, 1, 0.3f}, {0.3f, 0.3f, 1}, {1, 1, 0.3f}, {1, 0.3f, 1}, {0.3f, 1, 1}};
        int v = 0, ix = 0;
        for (int f = 0; f < 6; ++f)
        {
            const float* n = faces[f];
            float u[3] = {n[1], n[2], n[0]};
            float w[3] = {n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0]};
            for (int c = 0; c < 4; ++c)
            {
                float su = (c == 1 || c == 2) ? 1.f : -1.f;
                float sv = (c >= 2) ? 1.f : -1.f;
                for (int k = 0; k < 3; ++k)
                    verts[v].pos[k] = p * (n[k] + su * u[k] + sv * w[k]);
                memcpy(verts[v].color, colors[f], sizeof(verts[v].color));
                verts[v].uv[0] = su * 0.5f + 0.5f;
                verts[v].uv[1] = sv * 0.5f + 0.5f;
                ++v;
            }
            uint16_t b = (uint16_t)(f * 4);
            uint16_t quad[6] = {b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3)};
            for (int k = 0; k < 6; ++k)
                indices[ix++] = quad[k];
        }
        D3D12_DRAW_INDEXED_ARGUMENTS drawArgs{36, 2, 0, 0, 0};

        BeginUpload();
        vertexBuffer = CreateBufferWithData(verts, sizeof(verts), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, L"Cube vertices");
        if (evictMode)
            evictable = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, 32 * 1024 * 1024, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE, L"Evict: paged out and back");
        indexBuffer = CreateBufferWithData(indices, sizeof(indices), D3D12_RESOURCE_STATE_INDEX_BUFFER, L"Cube indices");
        indirectArgs = CreateBufferWithData(&drawArgs, sizeof(drawArgs), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, L"Draw arguments");
        CreateTexture();
        if (bindless)
            CreateStripesTexture();
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

        if (leak)
            CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, 4096, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, L"Leaked buffer").Detach();

        // Views into the shader-visible heap.
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = kColorFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = kTextureMips;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
            cbv.BufferLocation = constantBuffer->GetGPUVirtualAddress() + i * kConstantSlot;
            cbv.SizeInBytes = kConstantSlot;
            device->CreateConstantBufferView(&cbv, SrvCpuHandle(2 * i));
            // --ray-tracing rewrites this slot with the traced image once that exists
            // (CreateRayTracingResources), so the cubes show what the rays wrote.
            device->CreateShaderResourceView(texture.Get(), &srv, SrvCpuHandle(2 * i + 1));
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = kWaveCount;
        uav.Buffer.StructureByteStride = sizeof(float);
        device->CreateUnorderedAccessView(waveBuffer.Get(), nullptr, &uav, SrvCpuHandle(kHeapUav));
        if (bindless)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC stripes = srv;
            stripes.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(stripesTexture.Get(), &stripes, SrvCpuHandle(kHeapBindless));
        }
    }

    /** The cube's root constant `flags`: `base`, and --bindless's heap slot in bits 8 and up (cube.hlsl). */
    uint32_t CubeFlags(uint32_t base) const { return base | (bindless ? kHeapBindless << 8 : 0u); }

    // --bindless: a 4x4 texture of four colored stripes, one mip, which only the heap names.
    void CreateStripesTexture()
    {
        constexpr uint32_t kSize = 4;
        const uint8_t colors[kSize][4] = {{255, 80, 80, 255}, {80, 255, 80, 255}, {80, 80, 255, 255}, {255, 255, 80, 255}};
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = td.Height = kSize;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = kColorFormat;
        td.SampleDesc.Count = 1;
        CHECK(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&stripesTexture)));
        stripesTexture->SetName(L"Bindless stripes texture");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
        UINT rows;
        UINT64 rowSize, total;
        device->GetCopyableFootprints(&td, 0, 1, 0, &layout, &rows, &rowSize, &total);
        ComPtr<ID3D12Resource> staging = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ,
            D3D12_RESOURCE_FLAG_NONE, L"Stripes staging");
        uint8_t* mapped = nullptr;
        CHECK(staging->Map(0, nullptr, (void**)&mapped));
        for (UINT y = 0; y < rows; ++y)
            for (uint32_t x = 0; x < kSize; ++x)
                memcpy(mapped + layout.Offset + y * layout.Footprint.RowPitch + x * 4, colors[x], 4);
        staging->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = stripesTexture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = staging.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = layout;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b = Transition(stripesTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &b);
        uploads.push_back(staging);
    }

    // An 8x8 RGBA8 checkerboard with a mip chain, every level box-filtered from the one above on
    // the CPU and uploaded through one staging buffer with a CopyTextureRegion per level.
    void CreateTexture()
    {
        std::vector<uint8_t> levels[kTextureMips];
        levels[0].resize(kTextureSize * kTextureSize * 4);
        for (uint32_t y = 0; y < kTextureSize; ++y)
            for (uint32_t x = 0; x < kTextureSize; ++x)
            {
                uint8_t c = ((x + y) & 1) ? 255 : 90;
                uint8_t* px = &levels[0][(y * kTextureSize + x) * 4];
                px[0] = px[1] = px[2] = c;
                px[3] = 255;
            }
        for (uint32_t level = 1; level < kTextureMips; ++level)
        {
            uint32_t src = kTextureSize >> (level - 1), dst = kTextureSize >> level;
            levels[level].resize(dst * dst * 4);
            for (uint32_t y = 0; y < dst; ++y)
                for (uint32_t x = 0; x < dst; ++x)
                    for (uint32_t k = 0; k < 4; ++k)
                    {
                        uint32_t sum = 0;
                        for (uint32_t dy = 0; dy < 2; ++dy)
                            for (uint32_t dx = 0; dx < 2; ++dx)
                                sum += levels[level - 1][((2 * y + dy) * src + 2 * x + dx) * 4 + k];
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
        for (uint32_t level = 0; level < kTextureMips; ++level)
        {
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
    void RecordBundles()
    {
        CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundleAllocator)));
        bundleAllocator->SetName(L"Bundle allocator");
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundleAllocator.Get(), pipeline.Get(),
                IID_PPV_ARGS(&bundles[i])));
            wchar_t name[32];
            swprintf(name, 32, L"Cube bundle %u", i);
            bundles[i]->SetName(name);
            ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
            bundles[i]->SetDescriptorHeaps(1, heaps);
            bundles[i]->SetGraphicsRootSignature(rootSignature.Get());
            bundles[i]->SetGraphicsRootDescriptorTable(0, SrvGpuHandle(2 * i));
            RootConstants constants{0.0f, CubeFlags(1u)};
            bundles[i]->SetGraphicsRoot32BitConstants(1, sizeof(constants) / 4, &constants, 0);
            bundles[i]->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            bundles[i]->IASetVertexBuffers(0, 1, &vertexBufferView);
            bundles[i]->IASetIndexBuffer(&indexBufferView);
            bundles[i]->DrawIndexedInstanced(36, 2, 0, 0, 0);
            CHECK(bundles[i]->Close());
        }
    }

    // ----------------------------------------------------------------------------- ray tracing
    //
    // The DXR half, laid out the way test/triangle --ray-tracing lays out the Vulkan one: one
    // triangle in a bottom level built once, a top level over two instances of it rebuilt every
    // frame, and a trace into a 256x256 image.
    //
    // Where D3D12 differs and the capture library has to keep up: an acceleration structure has no
    // handle, only the address a build wrote it to; a state object's shaders are named by 32-byte
    // identifiers the runtime hands out per export; and the binding table is a plain buffer whose
    // records the application lays out itself.

    static constexpr uint32_t kRecordSize = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;   // 32: one identifier
    // The hit records are strided wider than they need to be, as a real application's are once
    // they carry local root arguments, so the capture has a stride to walk by that is not the
    // identifier size.
    static constexpr uint32_t kHitRecordSize = 64;
    static constexpr uint32_t kTableAlign = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;     // 64
    static constexpr uint32_t kRaygenOffset = 0;
    static constexpr uint32_t kMissOffset = kTableAlign;
    static constexpr uint32_t kHitOffset = kMissOffset + kTableAlign;
    static constexpr uint32_t kTableSize = kHitOffset + 2 * kHitRecordSize;

    /**
     * Both shader-readable states. A descriptor table range is DATA_STATIC_WHILE_SET_AT_EXECUTE by
     * default, and the runtime then asks for both bits when the table is bound, whichever stage
     * actually reads it.
     */
    static constexpr D3D12_RESOURCE_STATES kShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    /** A UAV buffer an acceleration structure or a build's scratch lives in. */
    ComPtr<ID3D12Resource> CreateUavBuffer(uint64_t size, D3D12_RESOURCE_STATES state, const wchar_t* name)
    {
        return CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, size, state, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, name);
    }

    static D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = resource;
        return b;
    }

    /** The inputs of the top level build, which both the size query and the build itself need. */
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS TlasInputs()
    {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.NumDescs = kRtInstances;
        in.InstanceDescs = rt.instances
            ? rt.instances->GetGPUVirtualAddress() + (UINT64)frameIndex * kRtInstanceSlot * sizeof(D3D12_RAYTRACING_INSTANCE_DESC)
            : 0;
        return in;
    }

    D3D12_RAYTRACING_GEOMETRY_DESC TriangleGeometry()
    {
        D3D12_RAYTRACING_GEOMETRY_DESC g{};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.VertexCount = 3;
        g.Triangles.VertexBuffer.StartAddress = rt.vertices ? rt.vertices->GetGPUVirtualAddress() : 0;
        g.Triangles.VertexBuffer.StrideInBytes = 3 * sizeof(float);
        return g;
    }

    void CreateRayTracing()
    {
        if (!rayTracing)
            return;
        if (FAILED(device.As(&rt.device5)))
        {
            fprintf(stderr, "--ray-tracing: ID3D12Device5 is not available\n");
            exit(1);
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (FAILED(rt.device5->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))) ||
            options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
        {
            fprintf(stderr, "--ray-tracing: this device has no DXR\n");
            exit(1);
        }

        // The global root signature every shader of the state object sees: one table holding the
        // scene's SRV and the traced image's UAV, in the two heap slots after the wave buffer's.
        D3D12_DESCRIPTOR_RANGE1 ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER1 param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 2;
        param.DescriptorTable.pDescriptorRanges = ranges;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
        rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rsd.Desc_1_1.NumParameters = 1;
        rsd.Desc_1_1.pParameters = &param;
        rt.rootSignature = MakeRootSignature(rsd, L"Ray tracing root signature");
        if (localRoot)
            CreateLocalRoot();

        CreateStateObject();
        CreateRayTracingResources();
        WriteBindingTable();
    }

    /**
     * --local-root: the tinted hit group's local root signature and what its arguments point at. The
     * two buffers hold colors the hit shader adds together, so a record whose address or handle the
     * replay got wrong shows as a wrong color rather than as nothing.
     */
    void CreateLocalRoot()
    {
        D3D12_ROOT_PARAMETER1 params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.RegisterSpace = 1;
        params[0].Constants.Num32BitValues = 1;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor.ShaderRegister = 1;
        params[1].Descriptor.RegisterSpace = 1;
        D3D12_DESCRIPTOR_RANGE1 range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 2;
        range.RegisterSpace = 1;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &range;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
        rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rsd.Desc_1_1.NumParameters = 3;
        rsd.Desc_1_1.pParameters = params;
        rsd.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
        rt.localRootSignature = MakeRootSignature(rsd, L"Ray tracing local root signature");

        auto colorBuffer = [&](const float (&color)[4], const wchar_t* name) {
            ComPtr<ID3D12Resource> buffer = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ,
                D3D12_RESOURCE_FLAG_NONE, name);
            void* mapped = nullptr;
            CHECK(buffer->Map(0, nullptr, &mapped));
            memset(mapped, 0, 256);
            memcpy(mapped, color, sizeof(color));
            buffer->Unmap(0, nullptr);
            return buffer;
        };
        const float tint[4] = {0.2f, 0.9f, 0.4f, 1.0f};
        const float tableColor[4] = {0.8f, 0.1f, 0.6f, 1.0f};
        rt.localTint = colorBuffer(tint, L"RT local root CBV");
        rt.localTable = colorBuffer(tableColor, L"RT local table CBV");
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
        cbv.BufferLocation = rt.localTable->GetGPUVirtualAddress();
        cbv.SizeInBytes = 256;
        device->CreateConstantBufferView(&cbv, SrvCpuHandle(kHeapRtLocal));
    }

    /**
     * The state object: one DXIL library with its four exports, two hit groups over its two
     * closest hits, the payload and attribute sizes, the recursion limit and the global root
     * signature. The library's exports are left for the runtime to take wholesale (NumExports 0),
     * which is the case a capture cannot enumerate from the description alone.
     */
    void CreateStateObject()
    {
        static std::vector<char> library = ReadFile(ExeDir() + (localRoot ? "raytrace_local.cso" : "raytrace.cso"));

        D3D12_DXIL_LIBRARY_DESC lib{};
        lib.DXILLibrary = {library.data(), library.size()};

        D3D12_HIT_GROUP_DESC hitGroups[2]{};
        hitGroups[0].HitGroupExport = L"HitGroup";
        hitGroups[0].Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitGroups[0].ClosestHitShaderImport = L"ClosestHit";
        hitGroups[1].HitGroupExport = L"HitGroupTinted";
        hitGroups[1].Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitGroups[1].ClosestHitShaderImport = L"ClosestHitTinted";

        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
        shaderConfig.MaxPayloadSizeInBytes = 3 * sizeof(float);      // Payload::color
        shaderConfig.MaxAttributeSizeInBytes = 2 * sizeof(float);    // the barycentrics

        D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
        pipelineConfig.MaxTraceRecursionDepth = 1;

        D3D12_GLOBAL_ROOT_SIGNATURE globalRoot{rt.rootSignature.Get()};

        // --local-root: the local root signature, and an association putting it on the tinted hit
        // group only, so the table's other records stay an identifier alone.
        D3D12_LOCAL_ROOT_SIGNATURE localRootDesc{rt.localRootSignature.Get()};
        D3D12_STATE_SUBOBJECT subobjects[8] = {
            {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib},
            {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroups[0]},
            {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroups[1]},
            {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
            {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig},
            {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRoot},
        };
        UINT count = 6;
        LPCWSTR tintedExports[] = {L"HitGroupTinted"};
        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{};
        if (localRoot)
        {
            subobjects[count] = {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &localRootDesc};
            association.pSubobjectToAssociate = &subobjects[count];
            association.NumExports = 1;
            association.pExports = tintedExports;
            ++count;
            subobjects[count++] = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &association};
        }
        D3D12_STATE_OBJECT_DESC desc{};
        desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        desc.NumSubobjects = count;
        desc.pSubobjects = subobjects;
        CHECK(rt.device5->CreateStateObject(&desc, IID_PPV_ARGS(&rt.stateObject)));
        rt.stateObject->SetName(L"Ray tracing state object");
    }

    void CreateRayTracingResources()
    {
        // The one triangle, in a buffer the build reads by address.
        const float triangle[9] = {-0.4f, -0.4f, 0.0f, 0.4f, -0.4f, 0.0f, 0.0f, 0.4f, 0.0f};
        BeginUpload();
        rt.vertices = CreateBufferWithData(triangle, sizeof(triangle),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"RT triangle");
        EndUpload();

        // How big each level and the scratch have to be, which only the runtime can say.
        D3D12_RAYTRACING_GEOMETRY_DESC geometry = TriangleGeometry();
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs{};
        blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        blasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blasInputs.NumDescs = 1;
        blasInputs.pGeometryDescs = &geometry;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasInfo{};
        rt.device5->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs, &blasInfo);

        // The instances, mapped and rewritten every frame so the two triangles turn.
        const size_t instanceBytes = (size_t)kFrameCount * kRtInstanceSlot * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        rt.instances = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, instanceBytes,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, L"RT instances");
        CHECK(rt.instances->Map(0, nullptr, (void**)&rt.instancesMapped));
        memset(rt.instancesMapped, 0, instanceBytes);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs = TlasInputs();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo{};
        rt.device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasInfo);

        rt.blas = CreateUavBuffer(blasInfo.ResultDataMaxSizeInBytes,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"RT triangle BLAS");
        rt.tlas = CreateUavBuffer(tlasInfo.ResultDataMaxSizeInBytes,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"RT scene TLAS");
        const uint64_t scratchSize = blasInfo.ScratchDataSizeInBytes > tlasInfo.ScratchDataSizeInBytes
            ? blasInfo.ScratchDataSizeInBytes
            : tlasInfo.ScratchDataSizeInBytes;
        rt.scratch = CreateUavBuffer(scratchSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RT build scratch");

        // The image the rays write. Nothing reads it: a capture is what looks at it.
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kTraceSize;
        td.Height = kTraceSize;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = kColorFormat;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        CHECK(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, IID_PPV_ARGS(&rt.target)));
        rt.target->SetName(L"RT traced image");

        // The scene's SRV: a view with no resource, naming the top level by address, and the
        // image's UAV beside it so the two are one table.
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.RaytracingAccelerationStructure.Location = rt.tlas->GetGPUVirtualAddress();
        device->CreateShaderResourceView(nullptr, &srv, SrvCpuHandle(kHeapRtScene));

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = kColorFormat;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(rt.target.Get(), nullptr, &uav, SrvCpuHandle(kHeapRtTarget));

        // The cubes sample the traced image instead of the checker, so the frame's one render
        // target depends on what the rays wrote.
        D3D12_SHADER_RESOURCE_VIEW_DESC cubeSrv{};
        cubeSrv.Format = kColorFormat;
        cubeSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        cubeSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        cubeSrv.Texture2D.MipLevels = 1;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            device->CreateShaderResourceView(rt.target.Get(), &cubeSrv, SrvCpuHandle(2 * i + 1));
        }
    }

    /**
     * The shader binding table: one raygen record, one miss record and two hit group records, each
     * beginning with the 32 bytes the runtime gave for its export. Which shader a ray runs is
     * decided entirely by these bytes and by the offsets a trace passes, which is why a capture
     * that cannot read this buffer can say nothing about what ran.
     */
    void WriteBindingTable()
    {
        ComPtr<ID3D12StateObjectProperties> properties;
        CHECK(rt.stateObject.As(&properties));

        rt.bindingTable = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, kTableSize, D3D12_RESOURCE_STATE_GENERIC_READ,
            D3D12_RESOURCE_FLAG_NONE, L"RT shader binding table");
        rt.tableAddress = rt.bindingTable->GetGPUVirtualAddress();
        uint8_t* mapped = nullptr;
        CHECK(rt.bindingTable->Map(0, nullptr, (void**)&mapped));
        memset(mapped, 0, kTableSize);
        auto put = [&](uint32_t offset, const wchar_t* exportName) {
            void* identifier = properties->GetShaderIdentifier(exportName);
            if (!identifier)
            {
                fprintf(stderr, "--ray-tracing: the state object has no export to put in the table\n");
                exit(1);
            }
            memcpy(mapped + offset, identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        };
        put(kRaygenOffset, L"RayGen");
        put(kMissOffset, L"Miss");
        put(kHitOffset, L"HitGroup");
        put(kHitOffset + kHitRecordSize, L"HitGroupTinted");
        if (localRoot)
        {
            // The tinted record's local root arguments, after its identifier, each at its own
            // alignment: the constant at 32, the root CBV's address at 40, the table's handle at 48.
            uint8_t* record = mapped + kHitOffset + kHitRecordSize;
            const float scale = 0.75f;
            const D3D12_GPU_VIRTUAL_ADDRESS tint = rt.localTint->GetGPUVirtualAddress();
            const D3D12_GPU_DESCRIPTOR_HANDLE table = SrvGpuHandle(kHeapRtLocal);
            memcpy(record + 32, &scale, sizeof(scale));
            memcpy(record + 40, &tint, sizeof(tint));
            memcpy(record + 48, &table.ptr, sizeof(table.ptr));
        }
        rt.bindingTable->Unmap(0, nullptr);
    }

    /**
     * The frame's ray tracing work: the bottom level once, the top level every frame, then the
     * trace. The UAV barriers between them are not optional -- a build and what reads it are both
     * unordered access, and without them the trace can run against a structure that is not there
     * yet (test/triangle's Vulkan version had exactly that defect, and every ray missed).
     */
    void RecordRayTracing(float t)
    {
        if (!rayTracing)
            return;
        ComPtr<ID3D12GraphicsCommandList4> rtList;
        if (FAILED(list.As(&rtList)))
            return;

        D3D12_RAYTRACING_GEOMETRY_DESC geometry = TriangleGeometry();
        if (!rt.built)
        {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.DestAccelerationStructureData = rt.blas->GetGPUVirtualAddress();
            build.ScratchAccelerationStructureData = rt.scratch->GetGPUVirtualAddress();
            build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            build.Inputs.NumDescs = 1;
            build.Inputs.pGeometryDescs = &geometry;
            rtList->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
            D3D12_RESOURCE_BARRIER done = UavBarrier(rt.blas.Get());
            rtList->ResourceBarrier(1, &done);
            rt.built = !rebuildBlas;
        }

        // The two instances, moved apart and turning. The second one contributes 1 to the hit
        // group index, so its rays run the second record of the table and come out a flat color.
        const D3D12_GPU_VIRTUAL_ADDRESS blasAddress = rt.blas->GetGPUVirtualAddress();
        for (uint32_t i = 0; i < kRtInstances; ++i)
        {
            D3D12_RAYTRACING_INSTANCE_DESC& instance = rt.instancesMapped[frameIndex * kRtInstanceSlot + i];
            const float angle = t * (i ? -0.8f : 0.6f);
            const float c = cosf(angle), sn = sinf(angle);
            // Row-major 3x4, the same layout as Vulkan's VkAccelerationStructureInstanceKHR.
            instance.Transform[0][0] = c;
            instance.Transform[0][1] = -sn;
            instance.Transform[0][2] = 0;
            instance.Transform[0][3] = i ? 0.45f : -0.45f;
            instance.Transform[1][0] = sn;
            instance.Transform[1][1] = c;
            instance.Transform[1][2] = 0;
            instance.Transform[1][3] = 0;
            instance.Transform[2][0] = 0;
            instance.Transform[2][1] = 0;
            instance.Transform[2][2] = 1;
            instance.Transform[2][3] = 0;
            instance.InstanceID = i;
            instance.InstanceMask = 0xFF;
            instance.InstanceContributionToHitGroupIndex = i;
            instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            instance.AccelerationStructure = blasAddress;
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.DestAccelerationStructureData = rt.tlas->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = rt.scratch->GetGPUVirtualAddress();
        build.Inputs = TlasInputs();
        rtList->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        D3D12_RESOURCE_BARRIER built = UavBarrier(rt.tlas.Get());
        rtList->ResourceBarrier(1, &built);

        D3D12_DISPATCH_RAYS_DESC trace{};
        trace.RayGenerationShaderRecord = {rt.tableAddress + kRaygenOffset, kRecordSize};
        trace.MissShaderTable = {rt.tableAddress + kMissOffset, kRecordSize, kRecordSize};
        trace.HitGroupTable = {rt.tableAddress + kHitOffset, 2 * kHitRecordSize, kHitRecordSize};
        trace.Width = kTraceSize;
        trace.Height = kTraceSize;
        trace.Depth = 1;
        rtList->SetComputeRootSignature(rt.rootSignature.Get());
        rtList->SetComputeRootDescriptorTable(0, SrvGpuHandle(kHeapRtScene));
        rtList->SetPipelineState1(rt.stateObject.Get());
        rtList->DispatchRays(&trace);

        // The cubes sample what the rays wrote, so the image has to leave unordered access.
        D3D12_RESOURCE_BARRIER toRead = Transition(rt.target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            kShaderRead);
        list->ResourceBarrier(1, &toRead);

        // The graphics pipeline the rest of the frame draws with: SetPipelineState1 replaced it.
        list->SetPipelineState(pipeline.Get());
    }

    /** Puts the traced image back where the next frame's rays expect it, after the draw has read it. */
    void EndRayTracing()
    {
        if (!rayTracing || !rt.target)
            return;
        D3D12_RESOURCE_BARRIER toWrite = Transition(rt.target.Get(), kShaderRead,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toWrite);
    }

    // --------------------------------------------------------------------------------- frame
    static constexpr float kClearColor[4] = {0.1f, 0.1f, 0.15f, 1.0f};

    bool DrawFrame(float t)
    {
        if (resized && !Resize())
            return false;
        WaitForFrame(frameIndex);

        Mat4 proj = Perspective(1.0f, (float)width / (float)height, 0.1f, 20.0f);
        Mat4 view = Translate(0, 0, -4.0f);
        CubeConstants constants{Mul(proj, view), Mul(RotateY(t), RotateX(t * 0.7f))};
        memcpy(constantMapped + frameIndex * kConstantSlot, &constants, sizeof(constants));

        const uint32_t poolSlot = (uint32_t)(frameCount % kPoolSize);
        if (pool)
        {
            // Reset when it last ran (below), so recording starts with its first command.
            list = poolLists[poolSlot];
            if (frameCount < kPoolSize)
                CHECK(list->Reset(poolAllocators[poolSlot].Get(), pipeline.Get()));
            if (renderPass)
                CHECK(list.As(&list4));
        }
        else
        {
            CHECK(allocators[frameIndex]->Reset());
            CHECK(list->Reset(allocators[frameIndex].Get(), pipeline.Get()));
        }
        ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
        list->SetDescriptorHeaps(1, heaps);

        // The frame's ray tracing, before the render targets are touched (--ray-tracing).
        RecordRayTracing(t);

        // The wave dispatch on the compute queue (--async-compute), submitted ahead of the frame's
        // list so the two run side by side. The buffer is COMMON, which a buffer is promoted out of
        // and decays back to on its own, so no barrier is needed on either queue.
        if (asyncCompute)
        {
            CHECK(computeAllocators[frameIndex]->Reset());
            CHECK(computeList->Reset(computeAllocators[frameIndex].Get(), computePipeline.Get()));
            ID3D12DescriptorHeap* computeHeaps[] = {srvHeap.Get()};
            computeList->SetDescriptorHeaps(1, computeHeaps);
            computeList->SetComputeRootSignature(computeRootSignature.Get());
            computeList->SetComputeRootDescriptorTable(0, SrvGpuHandle(kHeapUav));
            RootConstants params{t, kWaveCount};
            computeList->SetComputeRoot32BitConstants(1, sizeof(params) / 4, &params, 0);
            computeList->Dispatch(kWaveCount / 64, 1, 1);
            CHECK(computeList->Close());
            ID3D12CommandList* computeLists[] = {computeList.Get()};
            computeQueue->ExecuteCommandLists(1, computeLists);
            CHECK(computeQueue->Signal(computeFence.Get(), ++computeFenceValue));
        }
        // The wave dispatch, before the render targets are touched: a compute pass of its own.
        else if (compute)
        {
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
        if (idleState != backBufferState)
        {
            D3D12_RESOURCE_BARRIER toTarget = Transition(backBuffer, idleState, backBufferState);
            list->ResourceBarrier(1, &toTarget);
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvHandle(msaa ? kFrameCount : frameIndex);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        if (renderPass)
        {
            D3D12_RENDER_PASS_RENDER_TARGET_DESC rt{};
            rt.cpuDescriptor = rtv;
            rt.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
            rt.BeginningAccess.Clear.ClearValue.Format = kColorFormat;
            memcpy(rt.BeginningAccess.Clear.ClearValue.Color, kClearColor, sizeof(kClearColor));
            rt.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
            D3D12_RENDER_PASS_DEPTH_STENCIL_DESC ds{};
            ds.cpuDescriptor = dsv;
            ds.DepthBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
            ds.DepthBeginningAccess.Clear.ClearValue.Format = depthFormat;
            ds.DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth = 1.0f;
            ds.StencilBeginningAccess.Type = stencil ? D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR : D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
            ds.StencilBeginningAccess.Clear.ClearValue = ds.DepthBeginningAccess.Clear.ClearValue;
            ds.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
            ds.StencilEndingAccess.Type = stencil ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
            // --suspend: this half ends suspended and the next list resumes it, so both halves
            // preserve the targets -- what carries over is exactly what PRESERVE means. (NO_ACCESS
            // would say the view is not used by the pass at all, which the runtime rejects.)
            list4->BeginRenderPass(1, &rt, &ds, suspend ? D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS : D3D12_RENDER_PASS_FLAG_NONE);
        }
        else
        {
            list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            list->ClearRenderTargetView(rtv, kClearColor, 0, nullptr);
            list->ClearDepthStencilView(dsv, stencil ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        }
        if (stencil)
            list->OMSetStencilRef(1);

        D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
        D3D12_RECT scissor{0, 0, (LONG)width, (LONG)height};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetGraphicsRootSignature(rootSignature.Get());
        list->SetGraphicsRootDescriptorTable(0, SrvGpuHandle(2 * frameIndex));
        RootConstants frame{t, CubeFlags(0u)};
        list->SetGraphicsRoot32BitConstants(1, sizeof(frame) / 4, &frame, 0);
        if (bundle)
        {
            list->ExecuteBundle(bundles[frameIndex].Get());
        }
        else
        {
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &vertexBufferView);
            list->IASetIndexBuffer(&indexBufferView);
            if (indirect)
                list->ExecuteIndirect(commandSignature.Get(), 1, indirectArgs.Get(), 0, nullptr, 0);
            else
                list->DrawIndexedInstanced(36, 2, 0, 0, 0);
        }
        if (renderPass)
            list4->EndRenderPass();

        // Everything after the pass goes in whichever list is last: the frame's own, or the one
        // that resumed the suspended pass (--suspend).
        ID3D12GraphicsCommandList* tail = list.Get();
        if (suspend)
        {
            // The pass is suspended between these two lists: it ends in neither, and the runtime
            // rejects anything that generates GPU work in between -- which is what the capture
            // library has to record around (README.md, "Passes"). The queue runs them back to back
            // in one ExecuteCommandLists, which is what a suspended pass requires.
            EndRayTracing();
            CHECK(list->Close());
            CHECK(resumeAllocators[frameIndex]->Reset());
            CHECK(resumeList->Reset(resumeAllocators[frameIndex].Get(), pipeline.Get()));
            ID3D12DescriptorHeap* resumeHeaps[] = {srvHeap.Get()};
            resumeList->SetDescriptorHeaps(1, resumeHeaps);
            D3D12_RENDER_PASS_RENDER_TARGET_DESC rt{};
            rt.cpuDescriptor = rtv;
            rt.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
            rt.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
            D3D12_RENDER_PASS_DEPTH_STENCIL_DESC ds{};
            ds.cpuDescriptor = dsv;
            ds.DepthBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
            ds.StencilBeginningAccess.Type = stencil ? D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE
                                                     : D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
            ds.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
            ds.StencilEndingAccess.Type = stencil ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE
                                                  : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
            resumeList4->BeginRenderPass(1, &rt, &ds, D3D12_RENDER_PASS_FLAG_RESUMING_PASS);
            if (stencil)
                resumeList->OMSetStencilRef(1);
            resumeList->RSSetViewports(1, &viewport);
            resumeList->RSSetScissorRects(1, &scissor);
            resumeList->SetGraphicsRootSignature(rootSignature.Get());
            resumeList->SetGraphicsRootDescriptorTable(0, SrvGpuHandle(2 * frameIndex));
            // The second half's cube is tinted differently, so the two halves are told apart on
            // the screen as well as in the capture.
            RootConstants second{t, CubeFlags(1u)};
            resumeList->SetGraphicsRoot32BitConstants(1, sizeof(second) / 4, &second, 0);
            resumeList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            resumeList->IASetVertexBuffers(0, 1, &vertexBufferView);
            resumeList->IASetIndexBuffer(&indexBufferView);
            resumeList->DrawIndexedInstanced(36, 2, 0, 0, 0);
            resumeList4->EndRenderPass();
            tail = resumeList.Get();
        }

        if (msaa)
        {
            D3D12_RESOURCE_BARRIER toResolve = Transition(msaaTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            tail->ResourceBarrier(1, &toResolve);
            tail->ResolveSubresource(backBuffer, 0, msaaTarget.Get(), 0, kColorFormat);
            D3D12_RESOURCE_BARRIER after[2] = {
                Transition(msaaTarget.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                Transition(backBuffer, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_PRESENT),
            };
            tail->ResourceBarrier(2, after);
        }
        else if (idleState != D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            D3D12_RESOURCE_BARRIER toPresent = Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, idleState);
            tail->ResourceBarrier(1, &toPresent);
        }
        if (!suspend)
            EndRayTracing();

        CHECK(tail->Close());
        ID3D12CommandList* lists[] = {list.Get(), resumeList.Get()};
        queue->ExecuteCommandLists(suspend ? 2 : 1, lists);
        if (!offscreen)
            CHECK(swapChain->Present(1, 0));
        // The frame fence covers the compute queue's work too, so its allocator is free when the slot
        // comes round again (--async-compute).
        if (asyncCompute)
            CHECK(queue->Wait(computeFence.Get(), computeFenceValue));
        CHECK(queue->Signal(fence.Get(), nextFenceValue));
        fenceValues[frameIndex] = nextFenceValue++;
        if (pool)
        {
            // Back to the pool, reset at once: the allocator has to wait for the GPU, which a test
            // application can afford.
            WaitForGpu();
            CHECK(poolAllocators[poolSlot]->Reset());
            CHECK(poolLists[poolSlot]->Reset(poolAllocators[poolSlot].Get(), pipeline.Get()));
        }
        frameIndex = offscreen ? (frameIndex + 1) % kFrameCount : swapChain->GetCurrentBackBufferIndex();
        ++frameCount;
        return true;
    }

    void Cleanup()
    {
        WaitForGpu();
        churnRecent.clear();
        churnKept.clear();
        evictable.Reset();
        constantBuffer->Unmap(0, nullptr);
        CloseHandle(fenceEvent);
        // Everything else is released by the members' destructors, the device last.
    }

    int Run()
    {
        CreateWindowNative();
        InitDevice();
        CreateSwapChain();
        CreatePipelines();
        CreateResources();
        CreateRayTracing();
        if (bundle)
            RecordBundles();
        auto start = std::chrono::steady_clock::now();
        auto nextFrame = start;
        while (!quit && (maxFrames == 0 || frameCount < maxFrames))
        {
            PumpEvents();
            if (quit)
                break;
            float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
            if (stallMs)
                Sleep(stallMs);
            if (hitchEvery && frameCount > 0 && frameCount % hitchEvery == 0)
                Sleep(100);
            // Asked again each frame until somebody is there to hear it: the inspector connects a
            // few frames after the device is made.
            if (captureAt && frameCount >= captureAt && !captureAsked)
            {
                char label[48];
                snprintf(label, sizeof label, "asked at frame %llu", (unsigned long long)captureAt);   // the tab's name
                captureAsked = gpu_inspector_capture_named(1, label) != 0;
            }
            if (evictMode && evictable && frameCount > 0)
            {
                // Never drawn from, so evicting it is safe at any point of the frame.
                ID3D12Pageable* pageable = evictable.Get();
                if (frameCount % 120 == 0)
                    device->Evict(1, &pageable);
                else if (frameCount % 120 == 60)
                    device->MakeResident(1, &pageable);
            }
            if (churn)
            {
                churnRecent.push_back(CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, 64 * 1024, D3D12_RESOURCE_STATE_GENERIC_READ,
                    D3D12_RESOURCE_FLAG_NONE, L"Churn: per-frame scratch"));
                if (churnRecent.size() > 2)
                    churnRecent.erase(churnRecent.begin());
                if (frameCount % 30 == 0)
                {
                    churnKept.push_back(CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, 1024 * 1024, D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_FLAG_NONE, L"Churn: kept forever"));
                }
            }
            if (!DrawFrame(t))
                Sleep(16);
            // A swap chain paces the loop to the display; an offscreen renderer has nothing to
            // wait on and would spin a core at thousands of fps, so it is paced to ~60 the way a
            // real WebGPU application is driven by requestAnimationFrame.
            if (offscreen)
            {
                nextFrame += std::chrono::microseconds(16667);
                auto now = std::chrono::steady_clock::now();
                if (nextFrame > now)
                    std::this_thread::sleep_for(nextFrame - now);
                else
                    nextFrame = now;
            }
        }
        Cleanup();
        return 0;
    }
};

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    App app;
    int argc = __argc;
    char** argv = __argv;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            app.maxFrames = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc)
            app.width = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc)
            app.height = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stall") && i + 1 < argc)
            app.stallMs = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hitch-every") && i + 1 < argc)
            app.hitchEvery = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--capture-at") && i + 1 < argc)
            app.captureAt = (uint64_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--msaa"))
            app.msaa = true;
        else if (!strcmp(argv[i], "--bundle"))
            app.bundle = true;
        else if (!strcmp(argv[i], "--indirect"))
            app.indirect = true;
        else if (!strcmp(argv[i], "--render-pass"))
            app.renderPass = true;
        else if (!strcmp(argv[i], "--suspend"))
        {
            app.suspend = true;
            app.renderPass = true;   // a suspended pass is a render-pass-API pass
        }
        else if (!strcmp(argv[i], "--pool"))
            app.pool = true;
        else if (!strcmp(argv[i], "--compute"))
            app.compute = true;
        else if (!strcmp(argv[i], "--async-compute"))
        {
            app.asyncCompute = true;
            app.compute = true;   // the same dispatch, on a queue of its own
        }
        else if (!strcmp(argv[i], "--offscreen"))
            app.offscreen = true;
        else if (!strcmp(argv[i], "--leak"))
            app.leak = true;
        else if (!strcmp(argv[i], "--churn"))
            app.churn = true;
        else if (!strcmp(argv[i], "--evict"))
            app.evictMode = true;
        else if (!strcmp(argv[i], "--heavy"))
            app.heavy = true;
        else if (!strcmp(argv[i], "--ray-tracing"))
            app.rayTracing = true;
        else if (!strcmp(argv[i], "--rebuild-blas"))
        {
            app.rayTracing = true;
            app.rebuildBlas = true;
        }
        else if (!strcmp(argv[i], "--bindless"))
            app.bindless = true;
        else if (!strcmp(argv[i], "--local-root"))
        {
            app.rayTracing = true;
            app.localRoot = true;
        }
        else if (!strcmp(argv[i], "--debug-layer"))
            app.debugLayer = true;
        else if (!strcmp(argv[i], "--stencil"))
        {
            app.stencil = true;
            app.depthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        }
        else
        {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 1;
        }
    }
    return app.Run();
}
