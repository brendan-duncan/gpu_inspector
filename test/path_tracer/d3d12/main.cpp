// A Direct3D 12 path tracer: the final scene of "Ray Tracing in One Weekend"
// (https://raytracing.github.io/books/RayTracingInOneWeekend.html) on DXR, progressively
// accumulated one frame at a time. The counterpart of test/path_tracer/vulkan, and something to
// debug ray tracing with:
//
//   - three bottom-level structures of procedural primitives (one per material; their scratch at
//     three offsets into one buffer) under a top-level structure of three instances, each with an
//     InstanceID (its first sphere) and a hit group contribution (its material), with UAV barriers
//     between the builds and the trace that reads them;
//   - a raytracing state object built from one DXIL library, with an intersection shader shared by
//     three procedural hit groups, a closest hit shader per material, a miss shader and a ray
//     generation shader that walks each path in a loop; a global root signature that mixes a
//     descriptor table (the UAVs) with root SRVs, a root CBV and root constants;
//   - shader tables for DispatchRays, and a UAV that every frame reads and writes (the running
//     mean), so a frame depends on the frames before it; the output is copied to the back buffer.
//
// Usage: dxinsp_path_tracer [--frames N] [--width W] [--height H] [--spp N] [--depth N]
//                           [--no-accumulate] [--rebuild] [--debug-layer]
//
//   --spp N           samples per pixel per frame (1)
//   --depth N         bounces per path (50, the book's)
//   --no-accumulate   every frame stands alone: noisy, but independent of the frames before it
//   --rebuild         rebuild every acceleration structure in every frame, so a captured frame
//                     holds the builds as well as the trace
//   --debug-layer     the application enables the D3D12 debug layer itself
//
// The window is resizable; a new size starts the accumulation again.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../scene.h"

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

UINT64 AlignUp(UINT64 v, UINT64 a) {
    return (v + a - 1) / a * a;
}

std::wstring Widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

constexpr uint32_t M = rtiow::kMaterialCount;
constexpr uint32_t kFrameCount = 3;   // back buffers, and frames in flight
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kAccumulationFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;

// The library's exports.
constexpr const wchar_t* kRaygen = L"PathRayGen";
constexpr const wchar_t* kMiss = L"SkyMiss";
constexpr const wchar_t* kIntersection = L"SphereIntersection";
constexpr const wchar_t* kClosestHits[M] = {L"LambertianHit", L"MetalHit", L"DielectricHit"};
constexpr const wchar_t* kHitGroups[M] = {L"LambertianGroup", L"MetalGroup", L"DielectricGroup"};

// The global root signature's parameters.
enum RootParameter : UINT {
    kRootUavs,       // table: u0 the output, u1 the running mean
    kRootScene,      // SRV t0: the top-level structure
    kRootSpheres,    // SRV t1
    kRootCamera,     // CBV b0
    kRootFrame,      // constants b1: rtiow::FrameParams
    kRootCount,
};

struct App {
    uint32_t width = 960, height = 540;
    uint32_t maxFrames = 0;   // 0: until the window is closed
    uint32_t samplesPerFrame = 1;
    uint32_t maxDepth = 50;
    bool accumulate = true;
    bool rebuild = false;
    bool debugLayer = false;
    bool resized = false;

    HWND hwnd = nullptr;
    bool quit = false;

    // The device before everything made from it: members release in reverse order.
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12CommandAllocator> allocators[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList4> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    uint64_t fenceValues[kFrameCount]{};
    uint64_t nextFenceValue = 1;
    uint32_t frameIndex = 0;
    uint64_t frameCount = 0;
    uint32_t accumulated = 0;   // frames in the running mean

    ComPtr<ID3D12DescriptorHeap> uavHeap;   // shader visible: the output's and the running mean's UAVs
    UINT uavSize = 0;
    ComPtr<ID3D12Resource> backBuffers[kFrameCount];
    ComPtr<ID3D12Resource> output, accumulation;

    // The scene.
    rtiow::Scene scene;
    ComPtr<ID3D12Resource> spheres, camera;
    ComPtr<ID3D12Resource> aabbs[M];
    ComPtr<ID3D12Resource> blas[M];
    UINT64 blasScratchOffset[M]{};
    ComPtr<ID3D12Resource> instances, tlas, scratch;

    // The pipeline and its tables.
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12StateObject> stateObject;
    ComPtr<ID3D12Resource> shaderTable;
    D3D12_DISPATCH_RAYS_DESC dispatch{};

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
        wc.lpszClassName = "dxinsp_path_tracer";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassA(&wc);
        RECT r{0, 0, (LONG)width, (LONG)height};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowA(wc.lpszClassName, "GPU Inspector test: D3D12 path tracer", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
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
    static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        return b;
    }

    static D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = resource;
        return b;
    }

    ComPtr<ID3D12Resource> CreateBuffer(D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_STATES state,
                                        D3D12_RESOURCE_FLAGS flags, const std::wstring& name) {
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
        r->SetName(name.c_str());
        return r;
    }

    // An upload-heap buffer holding the data. The path tracer's inputs are small, and a mapped
    // buffer is what the acceleration structure builds and the shader tables read in the samples.
    ComPtr<ID3D12Resource> CreateUploadBuffer(const void* data, UINT64 size, const std::wstring& name) {
        ComPtr<ID3D12Resource> r = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                D3D12_RESOURCE_FLAG_NONE, name);
        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        CHECK(r->Map(0, &none, &mapped));
        memcpy(mapped, data, (size_t)size);
        r->Unmap(0, nullptr);
        return r;
    }

    ComPtr<ID3D12Resource> CreateUavTexture(DXGI_FORMAT format, const wchar_t* name) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width;
        rd.Height = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = format;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;
        CHECK(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                              IID_PPV_ARGS(&r)));
        r->SetName(name);
        return r;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE UavCpuHandle(UINT index) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = uavHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)index * uavSize;
        return h;
    }

    void BeginList(uint32_t slot) {
        CHECK(allocators[slot]->Reset());
        CHECK(list->Reset(allocators[slot].Get(), nullptr));
    }

    void ExecuteAndWait() {
        CHECK(list->Close());
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitForGpu();
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

        // The first hardware adapter with DXR, fastest first where the factory can order them.
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
            ComPtr<ID3D12Device5> d;
            if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d)))) continue;
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
            if (FAILED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))) ||
                options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
                continue;
            device = d;
            printf("adapter: %ls\n", desc.Description);
            break;
        }
        if (!device) {
            fprintf(stderr, "no Direct3D 12 adapter with DXR\n");
            exit(1);
        }
        device->SetName(L"Device");

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
        queue->SetName(L"Direct queue");

        for (uint32_t i = 0; i < kFrameCount; ++i) {
            CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])));
            allocators[i]->SetName((L"Frame allocator " + std::to_wstring(i)).c_str());
        }
        // Created closed: every use reopens it with Reset.
        CHECK(device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&list)));
        list->SetName(L"Frame command list");

        CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        fence->SetName(L"Frame fence");
        fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&uavHeap)));
        uavHeap->SetName(L"UAV heap");
        uavSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void CreateSwapChain() {
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
        CreateSizedResources();
    }

    // The back buffers, the output and the running mean at the window's size, with their UAVs,
    // and the camera for its aspect ratio. The accumulation starts again.
    void CreateSizedResources() {
        for (uint32_t i = 0; i < kFrameCount; ++i) {
            CHECK(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i])));
            backBuffers[i]->SetName((L"Back buffer " + std::to_wstring(i)).c_str());
        }
        frameIndex = swapChain->GetCurrentBackBufferIndex();

        output = CreateUavTexture(kColorFormat, L"Path tracer output");
        accumulation = CreateUavTexture(kAccumulationFormat, L"Path tracer accumulation");
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = kColorFormat;
        device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, UavCpuHandle(0));
        uav.Format = kAccumulationFormat;
        device->CreateUnorderedAccessView(accumulation.Get(), nullptr, &uav, UavCpuHandle(1));

        rtiow::Camera c = rtiow::MakeCamera(width, height);
        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        CHECK(camera->Map(0, &none, &mapped));
        memcpy(mapped, &c, sizeof(c));
        camera->Unmap(0, nullptr);
        accumulated = 0;
    }

    void ReleaseSizedResources() {
        for (auto& b : backBuffers) b.Reset();
        output.Reset();
        accumulation.Reset();
    }

    // Recreates the swap chain's buffers for the window's client area. Returns false when there
    // is nothing to draw into (minimized).
    bool Resize() {
        resized = false;
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
        return true;
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

    D3D12_RAYTRACING_GEOMETRY_DESC BlasGeometry(uint32_t m) {
        D3D12_RAYTRACING_GEOMETRY_DESC g{};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g.AABBs.AABBCount = scene.count[m];
        g.AABBs.AABBs.StartAddress = aabbs[m]->GetGPUVirtualAddress();
        g.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);
        return g;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS BlasInputs(const D3D12_RAYTRACING_GEOMETRY_DESC* geometry) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.NumDescs = 1;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = geometry;
        return in;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS TlasInputs() {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.NumDescs = M;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.InstanceDescs = instances->GetGPUVirtualAddress();
        return in;
    }

    // Sizes a structure and creates the buffer it lives in. Returns the build's scratch size.
    UINT64 CreateStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs, ComPtr<ID3D12Resource>& out,
                           const std::wstring& name) {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        UINT64 size = AlignUp(info.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        out = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, name);
        return AlignUp(info.ScratchDataSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    }

    void CreateScene() {
        scene = rtiow::MakeScene();
        printf("scene: %zu spheres (%u Lambertian, %u metal, %u dielectric)\n", scene.spheres.size(),
               scene.count[rtiow::kLambertian], scene.count[rtiow::kMetal], scene.count[rtiow::kDielectric]);
        spheres = CreateUploadBuffer(scene.spheres.data(), scene.spheres.size() * sizeof(rtiow::Sphere), L"Spheres");
        // A constant buffer's size is a multiple of 256.
        camera = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, AlignUp(sizeof(rtiow::Camera), 256), D3D12_RESOURCE_STATE_GENERIC_READ,
                              D3D12_RESOURCE_FLAG_NONE, L"Camera");

        // One scratch buffer: each bottom level has its own stretch of it, and the top level,
        // built once they are finished, starts at the beginning again.
        static_assert(sizeof(rtiow::Aabb) == sizeof(D3D12_RAYTRACING_AABB));
        UINT64 blasScratch = 0;
        for (uint32_t m = 0; m < M; ++m) {
            std::vector<rtiow::Aabb> boxes = rtiow::Bounds(scene, (rtiow::Material)m);
            std::wstring material = Widen(rtiow::MaterialName(m));
            aabbs[m] = CreateUploadBuffer(boxes.data(), boxes.size() * sizeof(rtiow::Aabb), material + L" AABBs");
            D3D12_RAYTRACING_GEOMETRY_DESC geometry = BlasGeometry(m);
            UINT64 size = CreateStructure(BlasInputs(&geometry), blas[m], material + L" BLAS");
            blasScratchOffset[m] = blasScratch;
            blasScratch += size;
        }

        // One instance per material: the identity transform, the material's first sphere as the
        // InstanceID and the material as the hit group contribution.
        D3D12_RAYTRACING_INSTANCE_DESC records[M]{};
        for (uint32_t m = 0; m < M; ++m) {
            D3D12_RAYTRACING_INSTANCE_DESC& r = records[m];
            r.Transform[0][0] = r.Transform[1][1] = r.Transform[2][2] = 1.0f;
            r.InstanceID = scene.first[m];
            r.InstanceMask = 0xFF;
            r.InstanceContributionToHitGroupIndex = m;
            r.AccelerationStructure = blas[m]->GetGPUVirtualAddress();
        }
        instances = CreateUploadBuffer(records, sizeof(records), L"Scene instances");
        UINT64 tlasScratch = CreateStructure(TlasInputs(), tlas, L"Scene TLAS");
        // Created in COMMON, like every buffer; the builds promote it to UNORDERED_ACCESS.
        scratch = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, std::max(blasScratch, tlasScratch), D3D12_RESOURCE_STATE_COMMON,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Build scratch");

        if (!rebuild) {
            BeginList(0);
            RecordBuilds();
            ExecuteAndWait();
        }
    }

    // The three bottom levels, then the top level over them. The UAV barriers are where the
    // runtime has to have finished the builds before them: the top level reads the bottom levels,
    // and the trace reads the top level.
    void RecordBuilds() {
        BeginMarker(L"Build acceleration structures");
        for (uint32_t m = 0; m < M; ++m) {
            D3D12_RAYTRACING_GEOMETRY_DESC geometry = BlasGeometry(m);
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
            desc.Inputs = BlasInputs(&geometry);
            desc.DestAccelerationStructureData = blas[m]->GetGPUVirtualAddress();
            desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress() + blasScratchOffset[m];
            list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
        }
        D3D12_RESOURCE_BARRIER barriers[M];
        for (uint32_t m = 0; m < M; ++m) barriers[m] = UavBarrier(blas[m].Get());
        list->ResourceBarrier(M, barriers);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        desc.Inputs = TlasInputs();
        desc.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
        desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
        D3D12_RESOURCE_BARRIER tlasBarrier = UavBarrier(tlas.Get());
        list->ResourceBarrier(1, &tlasBarrier);
        EndMarker();
    }

    void CreatePipeline() {
        // The global root signature.
        D3D12_DESCRIPTOR_RANGE1 uavRange{};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 2;
        uavRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;   // the running mean is read and written
        D3D12_ROOT_PARAMETER1 params[kRootCount]{};
        params[kRootUavs].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kRootUavs].DescriptorTable.NumDescriptorRanges = 1;
        params[kRootUavs].DescriptorTable.pDescriptorRanges = &uavRange;
        params[kRootScene].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[kRootScene].Descriptor.ShaderRegister = 0;
        params[kRootSpheres].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[kRootSpheres].Descriptor.ShaderRegister = 1;
        params[kRootCamera].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[kRootCamera].Descriptor.ShaderRegister = 0;
        params[kRootFrame].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[kRootFrame].Constants.ShaderRegister = 1;
        params[kRootFrame].Constants.Num32BitValues = sizeof(rtiow::FrameParams) / 4;
        for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
        rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rs.Desc_1_1.NumParameters = kRootCount;
        rs.Desc_1_1.pParameters = params;
        rootSignature = MakeRootSignature(rs, L"Path tracer root signature");

        // The state object: the library, three procedural hit groups, the shader and pipeline
        // configurations, and the root signature. Without associations, the configurations
        // apply to every export.
        std::vector<char> library = ReadFile(ExeDir() + "path_tracer.cso");
        std::vector<D3D12_STATE_SUBOBJECT> subobjects;
        subobjects.reserve(8);

        D3D12_DXIL_LIBRARY_DESC lib{};
        lib.DXILLibrary = {library.data(), library.size()};   // every export
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib});

        D3D12_HIT_GROUP_DESC hitGroups[M]{};
        for (uint32_t m = 0; m < M; ++m) {
            hitGroups[m].HitGroupExport = kHitGroups[m];
            hitGroups[m].Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
            hitGroups[m].ClosestHitShaderImport = kClosestHits[m];
            hitGroups[m].IntersectionShaderImport = kIntersection;
            subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroups[m]});
        }

        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
        shaderConfig.MaxPayloadSizeInBytes = 12 * sizeof(float);   // Payload: three float3s and three uints
        shaderConfig.MaxAttributeSizeInBytes = 3 * sizeof(float);   // SphereHit: the outward normal
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig});

        D3D12_GLOBAL_ROOT_SIGNATURE global{rootSignature.Get()};
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global});

        D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
        pipelineConfig.MaxTraceRecursionDepth = 1;   // the ray generation shader walks the path itself
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig});

        D3D12_STATE_OBJECT_DESC sod{};
        sod.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        sod.NumSubobjects = (UINT)subobjects.size();
        sod.pSubobjects = subobjects.data();
        CHECK(device->CreateStateObject(&sod, IID_PPV_ARGS(&stateObject)));
        stateObject->SetName(L"Path tracer pipeline");

        // The shader tables, in one buffer: [raygen] [miss] [Lambertian, metal, dielectric hit
        // groups]. No record has local arguments, so each is just the shader identifier.
        //
        // A record is 32 bytes but each *table* has to start on a 64-byte boundary
        // (D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT), so the three start at multiples of that
        // and not simply one after another. A buffer's own address is aligned far past it.
        ComPtr<ID3D12StateObjectProperties> properties;
        CHECK(stateObject.As(&properties));
        const UINT64 stride = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        const UINT64 tableAlign = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
        const UINT64 missOffset = AlignUp(stride, tableAlign);
        const UINT64 hitOffset = AlignUp(missOffset + stride, tableAlign);
        std::vector<uint8_t> table((size_t)(hitOffset + M * stride));
        auto identifier = [&](const wchar_t* name, UINT64 offset) {
            void* id = properties->GetShaderIdentifier(name);
            if (!id) {
                fprintf(stderr, "no shader identifier for %ls\n", name);
                exit(1);
            }
            memcpy(table.data() + offset, id, (size_t)stride);
        };
        identifier(kRaygen, 0);
        identifier(kMiss, missOffset);
        for (uint32_t m = 0; m < M; ++m) identifier(kHitGroups[m], hitOffset + m * stride);
        shaderTable = CreateUploadBuffer(table.data(), table.size(), L"Shader tables");

        D3D12_GPU_VIRTUAL_ADDRESS base = shaderTable->GetGPUVirtualAddress();
        dispatch.RayGenerationShaderRecord = {base, stride};
        dispatch.MissShaderTable = {base + missOffset, stride, stride};
        dispatch.HitGroupTable = {base + hitOffset, M * stride, stride};
        dispatch.Depth = 1;
    }

    // Markers with the metadata value 0 (a Unicode string), the form PIX and the D3D12 runtime
    // understand without WinPixEventRuntime.
    void BeginMarker(const wchar_t* name) {
        list->BeginEvent(0, name, (UINT)((wcslen(name) + 1) * sizeof(wchar_t)));
    }

    void EndMarker() {
        list->EndEvent();
    }

    // --------------------------------------------------------------------------------- frame
    bool DrawFrame() {
        if (resized && !Resize()) return false;
        WaitForFrame(frameIndex);
        BeginList(frameIndex);

        if (rebuild) RecordBuilds();

        rtiow::FrameParams params{accumulate ? accumulated : (uint32_t)frameCount, samplesPerFrame, maxDepth, accumulate ? 1u : 0u};
        BeginMarker(L"Path trace");
        ID3D12DescriptorHeap* heaps[] = {uavHeap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(rootSignature.Get());
        list->SetComputeRootDescriptorTable(kRootUavs, uavHeap->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootShaderResourceView(kRootScene, tlas->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(kRootSpheres, spheres->GetGPUVirtualAddress());
        list->SetComputeRootConstantBufferView(kRootCamera, camera->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstants(kRootFrame, sizeof(params) / 4, &params, 0);
        list->SetPipelineState1(stateObject.Get());
        dispatch.Width = width;
        dispatch.Height = height;
        list->DispatchRays(&dispatch);
        EndMarker();

        BeginMarker(L"Present");
        ID3D12Resource* backBuffer = backBuffers[frameIndex].Get();
        D3D12_RESOURCE_BARRIER before[2] = {
            Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        list->ResourceBarrier(2, before);
        list->CopyResource(backBuffer, output.Get());
        D3D12_RESOURCE_BARRIER after[2] = {
            Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Transition(backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
        };
        list->ResourceBarrier(2, after);
        EndMarker();

        CHECK(list->Close());
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        CHECK(swapChain->Present(1, 0));
        CHECK(queue->Signal(fence.Get(), nextFenceValue));
        fenceValues[frameIndex] = nextFenceValue++;
        frameIndex = swapChain->GetCurrentBackBufferIndex();
        ++frameCount;
        ++accumulated;
        if (frameCount % 100 == 0)
            printf("frame %llu: %u samples per pixel\n", (unsigned long long)frameCount, (accumulate ? accumulated : 1) * samplesPerFrame);
        return true;
    }

    int Run() {
        CreateWindowNative();
        InitDevice();
        CreateScene();   // before the swap chain: the sized resources write the camera
        CreateSwapChain();
        CreatePipeline();
        while (!quit && (maxFrames == 0 || frameCount < maxFrames)) {
            PumpEvents();
            if (quit) break;
            if (!DrawFrame()) Sleep(16);
        }
        WaitForGpu();
        CloseHandle(fenceEvent);
        return 0;
    }
};

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // a failure exits before a buffer would be flushed
    App app;
    int argc = __argc;
    char** argv = __argv;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) app.maxFrames = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) app.width = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) app.height = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--spp") && i + 1 < argc) app.samplesPerFrame = (uint32_t)std::max(1, atoi(argv[++i]));
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc) app.maxDepth = (uint32_t)std::max(1, atoi(argv[++i]));
        else if (!strcmp(argv[i], "--no-accumulate")) app.accumulate = false;
        else if (!strcmp(argv[i], "--rebuild")) app.rebuild = true;
        else if (!strcmp(argv[i], "--debug-layer")) app.debugLayer = true;
        else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 1;
        }
    }
    return app.Run();
}
