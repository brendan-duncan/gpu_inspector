// frame_window.h on Linux and the BSDs: Xlib, which a Wayland session serves through XWayland. It
// needs the X11 development headers (libx11-dev, libX11-devel); the project's CMakeLists.txt builds
// without a window, --batch only, where they are missing.
#include "frame_window.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

struct FrameWindow
{
    Display* display = nullptr;
    Window window = 0;
    Atom deleteMessage = 0;
    bool closed = false;
};

FrameWindow* OpenFrameWindow(const char* title, uint32_t width, uint32_t height)
{
    Display* display = XOpenDisplay(nullptr);
    if (!display)
        return nullptr;   // no display: a session over ssh, a build machine
    const int screen = DefaultScreen(display);
    auto* window = new FrameWindow;
    window->display = display;
    window->window = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, width, height, 0, BlackPixel(display, screen), BlackPixel(display, screen));
    if (!window->window)
    {
        XCloseDisplay(display);
        delete window;
        return nullptr;
    }
    // Not resizable: the swap chain is made once, for this size.
    if (XSizeHints* hints = XAllocSizeHints())
    {
        hints->flags = PMinSize | PMaxSize;
        hints->min_width = hints->max_width = (int)width;
        hints->min_height = hints->max_height = (int)height;
        XSetWMNormalHints(display, window->window, hints);
        XFree(hints);
    }
    XStoreName(display, window->window, title ? title : "");
    XSelectInput(display, window->window, KeyPressMask | StructureNotifyMask);
    // The close button sends a message instead of destroying the window under the swap chain.
    window->deleteMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window->window, &window->deleteMessage, 1);
    XMapWindow(display, window->window);
    XFlush(display);
    return window;
}

bool PumpFrameWindow(FrameWindow* window)
{
    if (!window)
        return false;
    while (XPending(window->display))
    {
        XEvent event;
        XNextEvent(window->display, &event);
        if (event.type == ClientMessage && (Atom)event.xclient.data.l[0] == window->deleteMessage)
            window->closed = true;
        else if (event.type == KeyPress && XLookupKeysym(&event.xkey, 0) == XK_Escape)
            window->closed = true;
        else if (event.type == DestroyNotify)
            window->closed = true;
    }
    return !window->closed;
}

void SetFrameWindowTitle(FrameWindow* window, const char* title)
{
    if (!window)
        return;
    XStoreName(window->display, window->window, title ? title : "");
    XFlush(window->display);
}

void CloseFrameWindow(FrameWindow* window)
{
    if (!window)
        return;
    XDestroyWindow(window->display, window->window);
    XCloseDisplay(window->display);
    delete window;
}

void* FrameWindowHandle(FrameWindow* window) { return window ? reinterpret_cast<void*>(static_cast<uintptr_t>(window->window)) : nullptr; }
void* FrameWindowDisplay(FrameWindow* window) { return window ? window->display : nullptr; }
