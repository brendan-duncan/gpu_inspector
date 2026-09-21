// The in-app HUD: the application's own frame time, drawn over the frame it is about to present.
//
// What it draws, and how the text becomes rectangles, is ../../vulkan/src/hud_text.h -- shared
// with the Vulkan layer and the D3D12 library, which is why the HUD reads the same on all three.
// This file is only the Metal way of putting those rectangles on the screen.
//
// Drawn from `presentDrawable:`, into the same command buffer the application is still encoding:
// the present it asks for there does not happen until that command buffer completes, so one more
// render pass appended to it, loading and storing the drawable's texture, lands on the frame
// before anyone sees it. No extra submission, no synchronization of our own -- the ordering is
// the command buffer's.
//
// The other way to present, `[MTLDrawable present]` called from a scheduled handler (Unity's macOS
// player does this; see the frame-boundary notes in capture.h), arrives after the command buffer
// that drew the frame has completed, so there is nothing left to append to. The HUD is skipped
// there, once with a line in the log. Live pause still works on that path: it does not need to
// draw anything.
//
// Metal compiles its shaders at run time from the source in hud.mm, which is why there is no
// generated header here as there is for Vulkan and D3D12.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <objc/objc.h>

#include "hud_text.h"

namespace mtlinsp {

class Hud {
public:
    static Hud& Get();

    /** From the UI (any thread), or MTLINSP_HUD at startup. */
    void SetEnabled(bool on);
    bool Enabled() const { return _enabled.load(std::memory_order_relaxed); }

    /**
     * Appends the HUD's render pass to `commandBuffer`, over `drawable`'s texture. Called from the
     * `presentDrawable:` hooks before the call is forwarded. Does nothing, quietly, if anything at
     * all goes wrong: a HUD that cannot be drawn must never cost the application its frame.
     */
    void DrawInto(id commandBuffer, id drawable);

    /** The application presents its drawables itself, so there is no command buffer to draw into. */
    void NoteUnsupportedPresentPath();

private:
    // One in-flight buffer of rectangles. Three of them: the HUD writes the next one while the GPU
    // may still be reading the last, and a drawable is at most triple buffered.
    struct Frame {
        id buffer = nil;        // id<MTLBuffer>, retained
        uint32_t capacity = 0;  // in rectangles
    };

    struct DeviceResources {
        id library = nil;          // id<MTLLibrary>, retained
        id vertexFunction = nil;   // id<MTLFunction>, retained
        id fragmentFunction = nil; // id<MTLFunction>, retained
        // One pipeline per drawable pixel format: a window moved to an HDR display changes it.
        std::unordered_map<uint64_t, id> pipelines;   // MTLPixelFormat -> id<MTLRenderPipelineState>
        std::vector<Frame> frames;
        size_t next = 0;
        bool failed = false;
        // The frame interval, measured between two of these draws: one fixed point in the
        // application's frame loop, so the interval is a whole frame.
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

    DeviceResources* Resources(id device);
    id PipelineFor(id device, DeviceResources& r, uint64_t pixelFormat);
    id BufferFor(id device, DeviceResources& r, uint32_t rects);
    void UpdateTiming(DeviceResources& r);

    std::atomic<bool> _enabled{false};
    std::atomic<bool> _warnedUnsupportedPath{false};
    std::mutex _mutex;
    std::unordered_map<const void*, DeviceResources> _devices;
};

}  // namespace mtlinsp
