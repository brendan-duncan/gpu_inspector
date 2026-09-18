// The in-app HUD: the application's own frame time, drawn over the frame it is about to present.
//
// What it draws, and how the text becomes rectangles, is ../../vulkan/src/hud_text.h -- shared
// with the Vulkan layer and the Metal library, which is why the HUD reads the same on all three.
// This file is only the D3D12 way of putting those rectangles on the screen.
//
// Drawn from the Present hook, before the real Present: the back buffer is in
// D3D12_RESOURCE_STATE_PRESENT by then (the application has to leave it there), so the overlay
// transitions it to a render target, draws, and puts it back.
//
// Ordering is simpler here than on Vulkan. DXGI places the present on the timeline of the queue
// the swap chain was created with, so a command list executed on that same queue before Present is
// called is already ordered before it -- no fence or semaphore is needed to keep the presentation
// engine from reading the image early, only the ring's own fence to know when a slot can be
// recorded into again.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "hud_text.h"

namespace dxinsp {

class Hud {
public:
    static Hud& Get();

    /** From the UI (any thread), or DXINSP_HUD at startup. */
    void SetEnabled(bool on);
    bool Enabled() const { return _enabled.load(std::memory_order_relaxed); }

    /**
     * Draws the HUD over the back buffer this present is about to show. Called before the real
     * Present, on the queue the swap chain presents from. Does nothing, quietly, if anything at
     * all goes wrong: a HUD that cannot be drawn must never cost the application its frame.
     */
    void Draw(ID3D12Device* device, IDXGISwapChain* swapChain, ID3D12CommandQueue* queue);

    /** Before ResizeBuffers: the render target views of the old back buffers are about to dangle. */
    void OnResizeBuffers(IDXGISwapChain* swapChain);
    /** The swap chain is going away. */
    void OnReleaseSwapChain(IDXGISwapChain* swapChain);

private:
    // Everything tied to one swap chain's back buffers: remade when they are.
    struct SwapChainResources {
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        std::vector<ComPtr<ID3D12Resource>> buffers;
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
        ComPtr<ID3D12PipelineState> pipeline;   // depends on the back buffer format
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t width = 0;
        uint32_t height = 0;
        bool usable = false;
    };

    // One in-flight overlay. Four of them, so the CPU never waits in practice.
    struct Frame {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Resource> vertices;   // an upload heap, mapped for the process's lifetime
        gpuhud::Rect* mapped = nullptr;
        uint32_t capacity = 0;
        uint64_t fenceValue = 0;
    };

    struct DeviceResources {
        ComPtr<ID3D12RootSignature> rootSignature;
        ComPtr<ID3D12Fence> fence;
        HANDLE event = nullptr;
        uint64_t nextFenceValue = 0;
        std::vector<Frame> frames;
        size_t next = 0;
        bool failed = false;
        // The frame interval, measured between two of these draws rather than taken from the
        // capture library's counters: one fixed point in the application's frame loop, so the
        // interval is a whole frame including the time Present itself blocks.
        std::chrono::steady_clock::time_point lastDraw{};
        uint64_t pauseGeneration = 0;
        double smoothedMs = 0;
        double minMs = 0;
        double maxMs = 0;
        double windowMs = 0;
        uint32_t windowFrames = 0;
        double shownMinMs = 0;
        double shownMaxMs = 0;
    };

    DeviceResources* Resources(ID3D12Device* device);
    SwapChainResources* Ensure(ID3D12Device* device, IDXGISwapChain* swapChain, DeviceResources& r);
    bool EnsureVertexBuffer(ID3D12Device* device, Frame& f, uint32_t rects);
    void UpdateTiming(DeviceResources& r);

    std::atomic<bool> _enabled{false};
    std::mutex _mutex;
    std::unordered_map<ID3D12Device*, DeviceResources> _devices;
    std::unordered_map<IDXGISwapChain*, SwapChainResources> _swapChains;
};

} // namespace dxinsp
