// Minimal Direct3D 11 test application, the counterpart of test/d3d12_triangle for the Direct3D 11
// plugin (src/plugins/d3d11). Each frame it
//   - renders a rotating textured cube into an offscreen color texture with a depth buffer: one
//     indexed draw from an immutable vertex buffer, a dynamic constant buffer written with Map,
//     a BC1 checkerboard with a full mip chain, and the fixed-function state objects (rasterizer,
//     depth stencil, blend, sampler);
//   - draws the window's back buffer: a fullscreen triangle that samples the offscreen texture,
//     mirrored and tinted so it is visibly a second pass; then presents.
// So a capture has a device and a swap chain, both kinds of context, every resource and view type
// a plain renderer uses, two passes with clears and event markers, and a present.
//
// Usage: d3d11insp_triangle [--frames N] [--width W] [--height H] [--capture-at N] [--msaa]
//                           [--deferred] [--compute] [--discard] [--debug-layer]
//
//   --frames N       exit after N presents (otherwise: when the window is closed)
//   --width W        the window's client width (default 640)
//   --height H       the window's client height (default 480)
//   --capture-at N   ask the inspector for a capture at frame N (include/gpu_inspector.h)
//   --msaa           the cube renders into 4x multisampled targets, resolved into the texture
//   --deferred       the cube pass is recorded on a deferred context and run as a command list
//   --compute        a compute shader fills a structured buffer between the passes
//   --discard        DiscardView on the depth buffer after the cube is drawn
//   --debug-layer    the application enables the D3D11 debug layer itself, and prints what it
//                    reported at exit
//
// The window is resizable: the swap chain's buffers and the offscreen targets are recreated when
// the client size changes. The shaders are HLSL strings below, compiled at run time with
// D3DCompile and debug information, so the inspector has source to show. Exit codes: 0 when the
// frames were drawn, 1 when Direct3D failed, 2 for a usage error or --help.

#include <windows.h>
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "gpu_inspector.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

const char kUsage[] =
    "Usage: d3d11insp_triangle [--frames N] [--width W] [--height H] [--capture-at N] [--msaa] "
    "[--deferred] [--compute] [--discard] [--debug-layer]\n";

// ------------------------------------------------------------------------------------------------
// Shaders. Column-major float4x4, HLSL's default packing: mul(mvp, v) is M * v, so the matrices
// below are built column-major.

const char kCubeHlsl[] = R"(
cbuffer Cube : register(b0) {
    float4x4 mvp;
    float4 lightDir;   // object space, toward the light
};
Texture2D checker : register(t0);
SamplerState linearSampler : register(s0);

struct VSIn {
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};
struct VSOut {
    float4 pos : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(VSIn i) {
    VSOut o;
    o.pos = mul(mvp, float4(i.pos, 1));
    o.normal = i.normal;
    o.uv = i.uv;
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    float diffuse = 0.3 + 0.7 * saturate(dot(normalize(i.normal), normalize(lightDir.xyz)));
    return float4(checker.Sample(linearSampler, i.uv).rgb * diffuse, 1);
}
)";

// The fullscreen triangle: positions from the vertex index, no vertex buffer. The image is
// mirrored left to right and tinted warm, so the window is visibly not the offscreen pass.
const char kPresentHlsl[] = R"(
Texture2D scene : register(t0);
SamplerState pointSampler : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv.x * 2 - 1, 1 - o.uv.y * 2, 0, 1);
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    float3 c = scene.Sample(pointSampler, float2(1 - i.uv.x, i.uv.y)).rgb;
    return float4(c * float3(1.0, 0.9, 0.8), 1);
}
)";

// --compute: 256 floats of a sine, one thread each, into a structured buffer nothing reads.
const char kWaveHlsl[] = R"(
RWStructuredBuffer<float> values : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    values[id.x] = sin(id.x * 0.1);
}
)";

// ------------------------------------------------------------------------------------------------

struct Vertex {
    float pos[3];
    float normal[3];
    float uv[2];
};

// Column-major: element (row, column) is m[column * 4 + row].
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

// The transpose of a rotation applied to a direction: world to object space, for the light.
void RotateInto(const Mat4& rotation, const float dir[3], float out[3]) {
    for (int row = 0; row < 3; ++row)
        out[row] = rotation.m[row * 4 + 0] * dir[0] + rotation.m[row * 4 + 1] * dir[1] + rotation.m[row * 4 + 2] * dir[2];
}

// The cube's cbuffer. 80 bytes, a multiple of the 16 a constant buffer's size must be.
struct CubeConstants {
    Mat4 mvp;
    float lightDir[4];
};

constexpr uint32_t kTextureSize = 64;
constexpr uint32_t kTextureMips = 7;     // 64x64 down to 1x1
constexpr uint32_t kCheckerCell = 8;     // pixels per checker cell at the top level
constexpr uint32_t kWaveCount = 256;     // --compute: floats in the structured buffer
constexpr uint32_t kBufferCount = 2;     // swap chain buffers
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
constexpr uint8_t kCheckerA[3] = {240, 200, 80};
constexpr uint8_t kCheckerB[3] = {40, 60, 160};
constexpr float kClearColor[4] = {0.1f, 0.12f, 0.2f, 1.0f};

uint16_t Rgb565(const uint8_t* c) {
    return (uint16_t)(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
}

// One mip level of the checkerboard as BC1 blocks. Each 4x4 block has the two checker colors as
// its endpoints and one index bit per pixel, which is all a two-color image needs of the format.
// The cells halve with each level; once they would be under a pixel the level is the average of
// the two colors, which is what filtering a checkerboard down gives. A level smaller than a block
// (2x2, 1x1) is still one whole block.
std::vector<uint8_t> EncodeCheckerBC1(uint32_t level) {
    const uint32_t size = std::max(1u, kTextureSize >> level);
    const uint32_t blocks = std::max(1u, size / 4);
    const uint32_t cell = kCheckerCell >> level;
    std::vector<uint8_t> data(blocks * blocks * 8);
    for (uint32_t by = 0; by < blocks; ++by)
        for (uint32_t bx = 0; bx < blocks; ++bx) {
            uint8_t* block = &data[(by * blocks + bx) * 8];
            uint16_t c0 = Rgb565(kCheckerA), c1 = Rgb565(kCheckerB);
            uint32_t indices = 0;   // 2 bits per pixel, row-major, pixel 0 in the low bits
            if (cell == 0) {
                uint8_t mid[3];
                for (int k = 0; k < 3; ++k) mid[k] = (uint8_t)((kCheckerA[k] + kCheckerB[k]) / 2);
                c0 = c1 = Rgb565(mid);
            } else {
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x) {
                        const uint32_t px = bx * 4 + x, py = by * 4 + y;
                        if (((px / cell) ^ (py / cell)) & 1) indices |= 1u << (2 * (y * 4 + x));
                    }
            }
            block[0] = (uint8_t)c0;
            block[1] = (uint8_t)(c0 >> 8);
            block[2] = (uint8_t)c1;
            block[3] = (uint8_t)(c1 >> 8);
            for (uint32_t k = 0; k < 4; ++k) block[4 + k] = (uint8_t)(indices >> (8 * k));
        }
    return data;
}

// A debug name, on anything with SetPrivateData: D3D11 objects, the device and DXGI objects alike.
template <typename T>
void Name(T* object, const char* name) {
    if (object) object->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)strlen(name), name);
}

// A context with the interfaces the passes want from it: the immediate context, or the deferred
// one with --deferred. Both are optional (Windows 8 and the platform update brought them) and
// are left null where they are missing.
struct Context {
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext1> ctx1;                 // --discard: DiscardView
    ComPtr<ID3DUserDefinedAnnotation> annotation;      // BeginEvent / EndEvent around each pass

    void Init(ID3D11DeviceContext* context, const char* name) {
        ctx = context;
        Name(ctx.Get(), name);
        ctx.As(&ctx1);
        ctx.As(&annotation);
    }

    void BeginEvent(const wchar_t* name) {
        if (annotation) annotation->BeginEvent(name);
    }

    void EndEvent() {
        if (annotation) annotation->EndEvent();
    }
};

struct App {
    uint32_t width = 640, height = 480;
    uint32_t maxFrames = 0;   // 0: until the window is closed
    uint64_t captureAt = 0;   // --capture-at: ask the inspector for a capture at this frame (gpu_inspector.h)
    bool captureAsked = false;
    // --msaa: the cube renders into a 4x multisampled color target and depth buffer, and the pass
    // ends with a ResolveSubresource into the single-sample texture the second pass samples.
    bool msaa = false;
    // --deferred: the cube pass is recorded on a deferred context every frame, closed with
    // FinishCommandList and run on the immediate context with ExecuteCommandList before the
    // second pass. A deferred context starts every recording with cleared state, so the pass
    // sets everything it needs each frame (it does so on the immediate context too).
    bool deferred = false;
    // --compute: between the passes a compute shader writes 256 floats into a structured buffer
    // through a UAV. Nothing reads it.
    bool compute = false;
    // --discard: after the cube is drawn, DiscardView on the depth stencil view: its contents are
    // not needed again (ID3D11DeviceContext1).
    bool discard = false;
    bool debugLayer = false;   // the application enables the D3D11 debug layer itself
    bool resized = false;      // the swap chain must be resized before the next frame

    HWND hwnd = nullptr;
    bool quit = false;
    uint64_t frameCount = 0;

    // The device before everything made from it: members release in reverse order, and the
    // device's last Release is what makes the inspector report leaks.
    ComPtr<ID3D11Device> device;
    Context immediate;
    Context deferredCtx;   // --deferred
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11RenderTargetView> backBufferRtv;

    // The offscreen targets, at the window's size.
    ComPtr<ID3D11Texture2D> sceneColor, sceneDepth, msaaColor;
    ComPtr<ID3D11RenderTargetView> sceneRtv, msaaRtv;
    ComPtr<ID3D11ShaderResourceView> sceneSrv;
    ComPtr<ID3D11DepthStencilView> sceneDsv;

    ComPtr<ID3D11VertexShader> cubeVs, presentVs;
    ComPtr<ID3D11PixelShader> cubePs, presentPs;
    ComPtr<ID3D11ComputeShader> waveCs;
    ComPtr<ID3D11InputLayout> inputLayout;

    ComPtr<ID3D11Buffer> vertexBuffer, indexBuffer, constantBuffer, waveBuffer;
    ComPtr<ID3D11UnorderedAccessView> waveUav;
    ComPtr<ID3D11Texture2D> checkerTexture;
    ComPtr<ID3D11ShaderResourceView> checkerSrv;
    ComPtr<ID3D11SamplerState> linearSampler, pointSampler;
    ComPtr<ID3D11RasterizerState> cullBack, cullNone;
    ComPtr<ID3D11DepthStencilState> depthOn, depthOff;
    ComPtr<ID3D11BlendState> noBlend;

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
        wc.lpszClassName = "d3d11insp_triangle";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassA(&wc);
        RECT r{0, 0, (LONG)width, (LONG)height};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowA(wc.lpszClassName, "GPU Inspector test: D3D11 cube", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
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

    // --------------------------------------------------------------------------------- setup
    void InitDevice() {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        if (debugLayer) flags |= D3D11_CREATE_DEVICE_DEBUG;
        const D3D_FEATURE_LEVEL wanted = D3D_FEATURE_LEVEL_11_0;
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_9_1;
        ComPtr<ID3D11DeviceContext> context;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &wanted, 1, D3D11_SDK_VERSION,
                                       &device, &got, &context);
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING && debugLayer) {
            fprintf(stderr, "--debug-layer: the D3D11 debug layer is not installed (Windows' Graphics Tools); running without\n");
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &wanted, 1, D3D11_SDK_VERSION,
                                   &device, &got, &context);
        }
        if (FAILED(hr)) {
            fprintf(stderr, "no Direct3D 11 hardware device at feature level 11.0: 0x%08lx\n", (unsigned long)hr);
            exit(1);
        }
        Name(device.Get(), "Device");
        immediate.Init(context.Get(), "Immediate context");

        if (deferred) {
            ComPtr<ID3D11DeviceContext> d;
            CHECK(device->CreateDeferredContext(0, &d));
            deferredCtx.Init(d.Get(), "Deferred context");
        }
        if (discard && !(deferred ? deferredCtx : immediate).ctx1) {
            fprintf(stderr, "--discard: ID3D11DeviceContext1 is not available (Windows 8 or newer)\n");
            exit(1);
        }

        // The factory the device's adapter came from, so the swap chain is made by the same DXGI
        // instance.
        ComPtr<IDXGIDevice> dxgiDevice;
        CHECK(device.As(&dxgiDevice));
        ComPtr<IDXGIAdapter> adapter;
        CHECK(dxgiDevice->GetAdapter(&adapter));
        CHECK(adapter->GetParent(IID_PPV_ARGS(&factory)));
    }

    void CreateSwapChain() {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = width;
        sd.Height = height;
        sd.Format = kColorFormat;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBufferCount;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        CHECK(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sd, nullptr, nullptr, &swapChain));
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        Name(swapChain.Get(), "Swap chain");
        CreateSizedResources();
    }

    // The back buffer's RTV, the offscreen color texture with its views, the depth buffer and
    // (--msaa) the multisampled color target, for the current window size.
    void CreateSizedResources() {
        ComPtr<ID3D11Texture2D> backBuffer;
        CHECK(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
        Name(backBuffer.Get(), "Back buffer");
        CHECK(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &backBufferRtv));
        Name(backBufferRtv.Get(), "Back buffer RTV");

        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = kColorFormat;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        CHECK(device->CreateTexture2D(&td, nullptr, &sceneColor));
        Name(sceneColor.Get(), "Scene color");
        CHECK(device->CreateRenderTargetView(sceneColor.Get(), nullptr, &sceneRtv));
        Name(sceneRtv.Get(), "Scene color RTV");
        CHECK(device->CreateShaderResourceView(sceneColor.Get(), nullptr, &sceneSrv));
        Name(sceneSrv.Get(), "Scene color SRV");

        td.Format = kDepthFormat;
        td.SampleDesc.Count = msaa ? 4 : 1;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        CHECK(device->CreateTexture2D(&td, nullptr, &sceneDepth));
        Name(sceneDepth.Get(), "Scene depth");
        CHECK(device->CreateDepthStencilView(sceneDepth.Get(), nullptr, &sceneDsv));
        Name(sceneDsv.Get(), "Scene depth DSV");

        if (msaa) {
            td.Format = kColorFormat;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            CHECK(device->CreateTexture2D(&td, nullptr, &msaaColor));
            Name(msaaColor.Get(), "Scene color (4x MSAA)");
            CHECK(device->CreateRenderTargetView(msaaColor.Get(), nullptr, &msaaRtv));
            Name(msaaRtv.Get(), "Scene color RTV (4x MSAA)");
        }
    }

    void ReleaseSizedResources() {
        backBufferRtv.Reset();
        sceneColor.Reset();
        sceneRtv.Reset();
        sceneSrv.Reset();
        sceneDepth.Reset();
        sceneDsv.Reset();
        msaaColor.Reset();
        msaaRtv.Reset();
    }

    // Recreates the swap chain's buffers and the offscreen targets for the window's client area.
    // Returns false when there is nothing to draw into (minimized).
    bool Resize() {
        resized = false;
        RECT r;
        GetClientRect(hwnd, &r);
        uint32_t w = (uint32_t)(r.right - r.left), h = (uint32_t)(r.bottom - r.top);
        if (w == 0 || h == 0) return false;
        if (w == width && h == height) return true;
        // ResizeBuffers refuses while anything still references the back buffer: the output
        // merger's binding and the RTV.
        immediate.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ReleaseSizedResources();
        width = w;
        height = h;
        CHECK(swapChain->ResizeBuffers(kBufferCount, width, height, DXGI_FORMAT_UNKNOWN, 0));
        CreateSizedResources();
        return true;
    }

    // Run-time compilation with debug information, so the bytecode carries the source and the
    // inspector can show it. `file` is the name the debug information records.
    ComPtr<ID3DBlob> Compile(const char* source, const char* file, const char* entry, const char* target) {
        ComPtr<ID3DBlob> code, errors;
        HRESULT hr = D3DCompile(source, strlen(source), file, nullptr, nullptr, entry, target, D3DCOMPILE_DEBUG, 0,
                                &code, &errors);
        if (FAILED(hr)) {
            fprintf(stderr, "%s (%s): %s\n", file, entry, errors ? (const char*)errors->GetBufferPointer() : "?");
            exit(1);
        }
        return code;
    }

    void CreateShaders() {
        ComPtr<ID3DBlob> vs = Compile(kCubeHlsl, "cube.hlsl", "VSMain", "vs_5_0");
        ComPtr<ID3DBlob> ps = Compile(kCubeHlsl, "cube.hlsl", "PSMain", "ps_5_0");
        CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &cubeVs));
        Name(cubeVs.Get(), "Cube VS");
        CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &cubePs));
        Name(cubePs.Get(), "Cube PS");

        // The vertex layout, validated against the cube vertex shader's input signature.
        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        CHECK(device->CreateInputLayout(layout, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &inputLayout));
        Name(inputLayout.Get(), "Cube input layout");

        vs = Compile(kPresentHlsl, "present.hlsl", "VSMain", "vs_5_0");
        ps = Compile(kPresentHlsl, "present.hlsl", "PSMain", "ps_5_0");
        CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &presentVs));
        Name(presentVs.Get(), "Present VS");
        CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &presentPs));
        Name(presentPs.Get(), "Present PS");

        if (compute) {
            ComPtr<ID3DBlob> cs = Compile(kWaveHlsl, "wave.hlsl", "CSMain", "cs_5_0");
            CHECK(device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &waveCs));
            Name(waveCs.Get(), "Wave CS");
        }
    }

    void CreateResources() {
        // Cube geometry: six quads with their face normal, wound counter-clockwise seen from
        // outside.
        const float p = 0.5f;
        Vertex verts[24];
        uint16_t indices[36];
        const float faces[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        int v = 0, ix = 0;
        for (int f = 0; f < 6; ++f) {
            const float* n = faces[f];
            float u[3] = {n[1], n[2], n[0]};
            float w[3] = {n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0]};
            for (int c = 0; c < 4; ++c) {
                float su = (c == 1 || c == 2) ? 1.f : -1.f;
                float sv = (c >= 2) ? 1.f : -1.f;
                for (int k = 0; k < 3; ++k) verts[v].pos[k] = p * (n[k] + su * u[k] + sv * w[k]);
                memcpy(verts[v].normal, n, sizeof(verts[v].normal));
                verts[v].uv[0] = su * 0.5f + 0.5f;
                verts[v].uv[1] = sv * 0.5f + 0.5f;
                ++v;
            }
            uint16_t b = (uint16_t)(f * 4);
            uint16_t quad[6] = {b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3)};
            for (int k = 0; k < 6; ++k) indices[ix++] = quad[k];
        }

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(verts);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = verts;
        CHECK(device->CreateBuffer(&bd, &init, &vertexBuffer));
        Name(vertexBuffer.Get(), "Cube vertices");

        bd.ByteWidth = sizeof(indices);
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        init.pSysMem = indices;
        CHECK(device->CreateBuffer(&bd, &init, &indexBuffer));
        Name(indexBuffer.Get(), "Cube indices");

        // Written by the CPU every frame with Map(WRITE_DISCARD).
        bd.ByteWidth = sizeof(CubeConstants);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        CHECK(device->CreateBuffer(&bd, nullptr, &constantBuffer));
        Name(constantBuffer.Get(), "Cube constants");

        if (compute) {
            bd = {};
            bd.ByteWidth = kWaveCount * sizeof(float);
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = sizeof(float);
            CHECK(device->CreateBuffer(&bd, nullptr, &waveBuffer));
            Name(waveBuffer.Get(), "Wave buffer");
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = kWaveCount;
            CHECK(device->CreateUnorderedAccessView(waveBuffer.Get(), &ud, &waveUav));
            Name(waveUav.Get(), "Wave UAV");
        }

        CreateTexture();

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        CHECK(device->CreateSamplerState(&sd, &linearSampler));
        Name(linearSampler.Get(), "Linear sampler");
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        CHECK(device->CreateSamplerState(&sd, &pointSampler));
        Name(pointSampler.Get(), "Point sampler");

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_BACK;
        rd.FrontCounterClockwise = TRUE;   // the cube's faces wind CCW seen from outside
        rd.DepthClipEnable = TRUE;
        rd.MultisampleEnable = msaa;
        CHECK(device->CreateRasterizerState(&rd, &cullBack));
        Name(cullBack.Get(), "Cull back");
        rd.CullMode = D3D11_CULL_NONE;   // the fullscreen triangle
        rd.MultisampleEnable = FALSE;
        CHECK(device->CreateRasterizerState(&rd, &cullNone));
        Name(cullNone.Get(), "Cull none");

        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS;
        CHECK(device->CreateDepthStencilState(&dd, &depthOn));
        Name(depthOn.Get(), "Depth on");
        dd.DepthEnable = FALSE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        CHECK(device->CreateDepthStencilState(&dd, &depthOff));
        Name(depthOff.Get(), "Depth off");

        D3D11_BLEND_DESC bld{};
        bld.RenderTarget[0].BlendEnable = FALSE;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        CHECK(device->CreateBlendState(&bld, &noBlend));
        Name(noBlend.Get(), "No blend");
    }

    // The 64x64 BC1 checkerboard with every mip level, encoded on the CPU and handed to
    // CreateTexture2D as initial data, one subresource per level.
    void CreateTexture() {
        std::vector<uint8_t> levels[kTextureMips];
        D3D11_SUBRESOURCE_DATA init[kTextureMips]{};
        for (uint32_t level = 0; level < kTextureMips; ++level) {
            levels[level] = EncodeCheckerBC1(level);
            const uint32_t blocks = std::max(1u, std::max(1u, kTextureSize >> level) / 4);
            init[level].pSysMem = levels[level].data();
            init[level].SysMemPitch = blocks * 8;   // a row of blocks
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kTextureSize;
        td.Height = kTextureSize;
        td.MipLevels = kTextureMips;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_BC1_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        CHECK(device->CreateTexture2D(&td, init, &checkerTexture));
        Name(checkerTexture.Get(), "Checker texture (BC1)");
        CHECK(device->CreateShaderResourceView(checkerTexture.Get(), nullptr, &checkerSrv));
        Name(checkerSrv.Get(), "Checker SRV");
    }

    // --------------------------------------------------------------------------------- frame
    // Pass 1, on whichever context records it: the cube into the offscreen target. Sets every
    // piece of state it uses, since a deferred context has none to begin with.
    void RecordScene(Context& c, float t) {
        c.BeginEvent(L"Scene");

        Mat4 proj = Perspective(1.0f, (float)width / (float)height, 0.1f, 20.0f);
        Mat4 view = Translate(0, 0, -3.0f);
        Mat4 model = Mul(RotateY(t), RotateX(t * 0.7f));
        CubeConstants constants{Mul(Mul(proj, view), model), {}};
        // The light is fixed in the world, above and toward the viewer; the shader gets it in the
        // cube's space so the normals need no transform.
        const float light[3] = {0.4f, 0.8f, 0.5f};
        RotateInto(model, light, constants.lightDir);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CHECK(c.ctx->Map(constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        memcpy(mapped.pData, &constants, sizeof(constants));
        c.ctx->Unmap(constantBuffer.Get(), 0);

        ID3D11RenderTargetView* rtv = msaa ? msaaRtv.Get() : sceneRtv.Get();
        c.ctx->OMSetRenderTargets(1, &rtv, sceneDsv.Get());
        c.ctx->ClearRenderTargetView(rtv, kClearColor);
        c.ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        D3D11_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
        c.ctx->RSSetViewports(1, &viewport);
        c.ctx->RSSetState(cullBack.Get());
        c.ctx->OMSetDepthStencilState(depthOn.Get(), 0);
        c.ctx->OMSetBlendState(noBlend.Get(), nullptr, 0xFFFFFFFF);

        c.ctx->IASetInputLayout(inputLayout.Get());
        c.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* vb = vertexBuffer.Get();
        UINT stride = sizeof(Vertex), offset = 0;
        c.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        c.ctx->IASetIndexBuffer(indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
        ID3D11Buffer* cb = constantBuffer.Get();
        c.ctx->VSSetShader(cubeVs.Get(), nullptr, 0);
        c.ctx->VSSetConstantBuffers(0, 1, &cb);
        c.ctx->PSSetShader(cubePs.Get(), nullptr, 0);
        c.ctx->PSSetConstantBuffers(0, 1, &cb);
        ID3D11ShaderResourceView* srv = checkerSrv.Get();
        c.ctx->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState* sampler = linearSampler.Get();
        c.ctx->PSSetSamplers(0, 1, &sampler);
        c.ctx->DrawIndexed(36, 0, 0);

        // The depth is not needed after the draw (--discard); the multisampled color is resolved
        // into the texture the next pass samples (--msaa).
        if (discard && c.ctx1) c.ctx1->DiscardView(sceneDsv.Get());
        c.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        if (msaa) c.ctx->ResolveSubresource(sceneColor.Get(), 0, msaaColor.Get(), 0, kColorFormat);

        c.EndEvent();
    }

    // --compute, between the passes: a dispatch into the structured buffer, unbound again after.
    void RunCompute() {
        ID3D11DeviceContext* ctx = immediate.ctx.Get();
        immediate.BeginEvent(L"Wave");
        ctx->CSSetShader(waveCs.Get(), nullptr, 0);
        ID3D11UnorderedAccessView* uav = waveUav.Get();
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->Dispatch(kWaveCount / 64, 1, 1);
        ID3D11UnorderedAccessView* none = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
        ctx->CSSetShader(nullptr, nullptr, 0);
        immediate.EndEvent();
    }

    // Pass 2, always on the immediate context: the offscreen image onto the back buffer with a
    // fullscreen triangle, no depth, no vertex buffer.
    void DrawPresent() {
        ID3D11DeviceContext* ctx = immediate.ctx.Get();
        immediate.BeginEvent(L"Present");
        ID3D11RenderTargetView* rtv = backBufferRtv.Get();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
        ctx->RSSetViewports(1, &viewport);
        ctx->RSSetState(cullNone.Get());
        ctx->OMSetDepthStencilState(depthOff.Get(), 0);
        ctx->OMSetBlendState(noBlend.Get(), nullptr, 0xFFFFFFFF);

        ctx->IASetInputLayout(nullptr);
        ID3D11Buffer* noBuffer = nullptr;
        UINT zero = 0;
        ctx->IASetVertexBuffers(0, 1, &noBuffer, &zero, &zero);
        ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(presentVs.Get(), nullptr, 0);
        ctx->PSSetShader(presentPs.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv = sceneSrv.Get();
        ctx->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState* sampler = pointSampler.Get();
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->Draw(3, 0);

        // The offscreen texture is a render target again next frame: unbound here rather than
        // forcibly by OMSetRenderTargets then, which the debug layer reports.
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
        immediate.EndEvent();
    }

    bool DrawFrame(float t) {
        if (resized && !Resize()) return false;

        if (deferred) {
            RecordScene(deferredCtx, t);
            ComPtr<ID3D11CommandList> list;
            CHECK(deferredCtx.ctx->FinishCommandList(FALSE, &list));
            Name(list.Get(), "Scene command list");
            immediate.ctx->ExecuteCommandList(list.Get(), FALSE);
        } else {
            RecordScene(immediate, t);
        }
        if (compute) RunCompute();
        DrawPresent();

        CHECK(swapChain->Present(1, 0));
        ++frameCount;
        return true;
    }

    void Cleanup() {
        immediate.ctx->ClearState();
        immediate.ctx->Flush();
        // --debug-layer: what the layer had to say, which otherwise only a debugger sees.
        ComPtr<ID3D11InfoQueue> queue;
        if (debugLayer && SUCCEEDED(device.As(&queue))) {
            const UINT64 count = queue->GetNumStoredMessages();
            std::vector<char> buffer;
            for (UINT64 i = 0; i < count; ++i) {
                SIZE_T length = 0;
                if (FAILED(queue->GetMessage(i, nullptr, &length)) || length == 0) continue;
                buffer.resize(length);
                D3D11_MESSAGE* message = (D3D11_MESSAGE*)buffer.data();
                if (SUCCEEDED(queue->GetMessage(i, message, &length)))
                    fprintf(stderr, "debug layer: %.*s\n", (int)message->DescriptionByteLength, message->pDescription);
            }
        }
        // Everything else is released by the members' destructors, the device last.
    }

    int Run() {
        CreateWindowNative();
        InitDevice();
        CreateSwapChain();
        CreateShaders();
        CreateResources();
        auto start = std::chrono::steady_clock::now();
        while (!quit && (maxFrames == 0 || frameCount < maxFrames)) {
            PumpEvents();
            if (quit) break;
            float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
            // Asked again each frame until somebody is there to hear it: the inspector connects a
            // few frames after the device is made.
            if (captureAt && frameCount >= captureAt && !captureAsked) {
                char label[48];
                snprintf(label, sizeof label, "asked at frame %llu", (unsigned long long)captureAt);   // the tab's name
                captureAsked = gpu_inspector_capture_named(1, label) != 0;
            }
            if (!DrawFrame(t)) Sleep(16);
        }
        Cleanup();
        return 0;
    }
};

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // A windowed application has no console of its own; when started from one, its usage and
    // errors go there. A stream the parent redirected to a file or a pipe is left alone.
    const bool ownStdout = GetStdHandle(STD_OUTPUT_HANDLE) == nullptr;
    const bool ownStderr = GetStdHandle(STD_ERROR_HANDLE) == nullptr;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        if (ownStdout) freopen_s(&f, "CONOUT$", "w", stdout);
        if (ownStderr) freopen_s(&f, "CONOUT$", "w", stderr);
    }
    App app;
    int argc = __argc;
    char** argv = __argv;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) app.maxFrames = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) app.width = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) app.height = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--capture-at") && i + 1 < argc) app.captureAt = (uint64_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--msaa")) app.msaa = true;
        else if (!strcmp(argv[i], "--deferred")) app.deferred = true;
        else if (!strcmp(argv[i], "--compute")) app.compute = true;
        else if (!strcmp(argv[i], "--discard")) app.discard = true;
        else if (!strcmp(argv[i], "--debug-layer")) app.debugLayer = true;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fputs(kUsage, stdout);
            return 2;
        } else {
            fprintf(stderr, "unknown option %s\n%s", argv[i], kUsage);
            return 2;
        }
    }
    if (app.width == 0 || app.height == 0) {
        fprintf(stderr, "--width and --height must be positive\n%s", kUsage);
        return 2;
    }
    return app.Run();
}
