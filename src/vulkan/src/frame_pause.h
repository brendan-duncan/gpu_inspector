// Live pause: freezing the application at a frame boundary, and letting it go again a frame at a
// time.
//
// There is no graphics API in here -- it is a mutex, a condition variable and a step count -- so
// all three capture libraries share it, like hud_text.h beside it. Each one calls Wait() at its
// own frame boundary: vkQueuePresentKHR on Vulkan, IDXGISwapChain::Present on D3D12, the drawable's
// present on Metal.
//
// Where the wait goes matters. It is *after* the frame has been presented, not before, so the
// frame the user is looking at is the one the application last drew, complete, with the HUD's
// PAUSED line on it (the HUD draws earlier in the same present, and reads Paused() to know). A
// wait before the present would freeze on the previous frame and leave the one just rendered
// invisible.
//
// What pausing costs the application: its render thread stops inside the present call, so it stops
// pumping its window's messages too. After a few seconds Windows will mark the window as not
// responding and draw its ghost copy -- the frame is still what the compositor shows, but the
// title bar says so. That is inherent to freezing a running application and is the same thing
// Nsight's live pause does.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace gpuinsp
{

class FramePause
{
public:
    static FramePause& Get()
    {
        static FramePause* instance = new FramePause();
        return *instance;
    }

    /** True while the application is being held at frame boundaries. Cheap: the HUD reads it every frame. */
    bool Paused() const { return _paused.load(std::memory_order_relaxed); }

    /**
     * From the UI, on any thread. Resuming releases a thread already waiting.
     *
     * Pausing lets one more frame through on purpose. Most of a frame's wall time is spent inside
     * the present call itself -- under FIFO that is the wait for vblank -- so a pause request
     * usually arrives after the HUD has already drawn the frame in flight, and freezing on that
     * frame would leave the user looking at one with no PAUSED on it. Granting a step means the
     * next frame is drawn knowing it is paused, carries the badge, and is the one left on screen.
     */
    void SetPaused(bool paused)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _paused.store(paused, std::memory_order_relaxed);
            _steps = paused ? 1 : 0;
        }
        _cv.notify_all();
    }

    /**
     * Counts the times the application has actually been held here. A frame interval measured
     * across a change in this is the length of a pause rather than of a frame, which is how the
     * HUD knows to throw it away instead of reporting a five-second frame.
     */
    uint64_t Generation() const { return _generation.load(std::memory_order_relaxed); }

    /**
     * Lets `frames` more frames through and stays paused, which is how a single frame is stepped.
     * Requesting a step while running does nothing, since nothing is waiting.
     */
    void Step(uint32_t frames)
    {
        if (!frames)
            return;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _steps += frames;
        }
        _cv.notify_all();
    }

    /**
     * Called at the frame boundary, after the frame has been presented: blocks while paused, unless
     * a step is owed, in which case it takes one and lets this frame through.
     */
    void Wait()
    {
        std::unique_lock<std::mutex> lock(_mutex);
        bool blocked = false;
        while (_paused.load(std::memory_order_relaxed) && _steps == 0)
        {
            blocked = true;
            _cv.wait(lock);
        }
        if (_steps > 0)
            --_steps;
        if (blocked)
            _generation.fetch_add(1, std::memory_order_relaxed);
    }

private:
    mutable std::mutex _mutex;
    std::condition_variable _cv;
    std::atomic<bool> _paused{false};
    std::atomic<uint64_t> _generation{0};
    uint32_t _steps = 0;
};

} // namespace gpuinsp
