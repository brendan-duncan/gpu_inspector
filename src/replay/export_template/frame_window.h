// The window the exported frame is shown in: as little as it takes to put a swap chain on screen and
// to know when it was closed. One implementation per platform (frame_window_win32.cpp,
// frame_window_x11.cpp, frame_window_cocoa.mm), chosen by the project's CMakeLists.txt; nothing in
// here knows a graphics API. The window has the size of the frame's output and is not resizable, so
// the swap chain made for it is the only one.
#pragma once

#include <cstdint>

struct FrameWindow;

/** A window with a client area of this size, shown; null when there is no display to open one on. */
FrameWindow* OpenFrameWindow(const char* title, uint32_t width, uint32_t height);
/** Handles what the window system has for the window; false once it was closed (or Escape pressed). */
bool PumpFrameWindow(FrameWindow* window);
void SetFrameWindowTitle(FrameWindow* window, const char* title);
void CloseFrameWindow(FrameWindow* window);

/**
 * What a graphics API makes its surface from.
 *   Windows: the HWND, and the HINSTANCE as the display.
 *   X11:     the Window (an integer, in the pointer), and the Display*.
 *   macOS:   the CAMetalLayer* of the window's view, and null.
 */
void* FrameWindowHandle(FrameWindow* window);
void* FrameWindowDisplay(FrameWindow* window);
