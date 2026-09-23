// frame_window.h on Windows: a plain Win32 window.
#include "frame_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

struct FrameWindow
{
    HWND hwnd = nullptr;
    bool closed = false;
};

namespace
{

const wchar_t kClassName[] = L"GpuInspectorExportedFrame";

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* window = reinterpret_cast<FrameWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message)
    {
        case WM_CLOSE:
            if (window)
                window->closed = true;
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE && window)
                window->closed = true;
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

}  // namespace

FrameWindow* OpenFrameWindow(const char* title, uint32_t width, uint32_t height)
{
    // The frame's pixels are shown one to one, whatever the display's scale.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return nullptr;

    // Not resizable: the swap chain is made once, for this size.
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect{0, 0, (LONG)width, (LONG)height};
    AdjustWindowRect(&rect, style, FALSE);
    wchar_t wide[256];
    if (!MultiByteToWideChar(CP_UTF8, 0, title ? title : "", -1, wide, 256))
        wide[0] = 0;
    auto* window = new FrameWindow;
    window->hwnd = CreateWindowExW(0, kClassName, wide, style, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance, nullptr);
    if (!window->hwnd)
    {
        delete window;
        return nullptr;
    }
    SetWindowLongPtrW(window->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    ShowWindow(window->hwnd, SW_SHOWNORMAL);
    return window;
}

bool PumpFrameWindow(FrameWindow* window)
{
    if (!window)
        return false;
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return !window->closed;
}

void SetFrameWindowTitle(FrameWindow* window, const char* title)
{
    if (!window || !window->hwnd)
        return;
    wchar_t wide[256];
    if (MultiByteToWideChar(CP_UTF8, 0, title ? title : "", -1, wide, 256))
        SetWindowTextW(window->hwnd, wide);
}

void CloseFrameWindow(FrameWindow* window)
{
    if (!window)
        return;
    if (window->hwnd)
        DestroyWindow(window->hwnd);
    delete window;
}

void* FrameWindowHandle(FrameWindow* window) { return window ? window->hwnd : nullptr; }
void* FrameWindowDisplay(FrameWindow*) { return GetModuleHandleW(nullptr); }
