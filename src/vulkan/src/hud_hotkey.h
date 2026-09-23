// The HUD's capture hotkey: a key pressed in the application's own window takes a capture, so a
// frame can be grabbed without leaving the application to click Capture in the inspector -- which
// is the one thing clicking cannot do, since clicking away from a game is itself a change to the
// frame (a mouse-look game turns, a pause menu opens, an exclusive-fullscreen swap chain may even
// be lost).
//
// There is no graphics API in here, so all three capture libraries share it, like hud_text.h and
// frame_pause.h beside it. Each one polls it once per present, while the HUD is on, and turns a
// press into the same AppCaptureRequest the application's own gpu_inspector_capture sends: the
// inspector takes the capture with the capture bar's options, into a tab that is waiting for it.
//
// Armed with the HUD and not on its own, because the HUD is what tells the user the key is live:
// its "F11 CAPTURE" line is the only thing on the screen that says so, and a key that silently
// takes over F11 in every application the inspector ever attaches to would be a worse trade than
// one click to arm it.
//
// Three platform implementations, all of which answer one question -- has the key been pressed,
// in *this* application's window -- and the focus half of that question is the reason none of them
// is one line:
//
// - Windows polls GetAsyncKeyState at present time. It reads the global key state rather than the
//   window's messages, so it works whatever the application does with input (raw input,
//   DirectInput, an engine that never pumps a message loop), but for the same reason it would fire
//   while the user is typing in the inspector's own window: hence the foreground-process test.
// - Linux polls XQueryKeymap on a connection of its own, and asks the window manager which window
//   is active (_NET_ACTIVE_WINDOW) and whose it is (_NET_WM_PID). Xlib is dlopen'd rather than
//   linked, as in refresh_rate.cpp, so a Wayland-native or headless run simply has no hotkey.
// - macOS has no polling at all: an NSEvent local monitor sees only the key events delivered to
//   this application, which is exactly the focus rule the other two have to reconstruct, and needs
//   no accessibility permission the way a global monitor or an event tap would. It sets a flag the
//   next present consumes.
//
// The binding comes from the backend's VKINSP_HOTKEY / DXINSP_HOTKEY / MTLINSP_HOTKEY setting
// ("F9", "CTRL+F11", "off"); F11 is the default, the same key Nsight uses, and one of the few
// function keys neither Windows nor Steam has already taken. On macOS F11 is Show Desktop until
// that shortcut is turned off, so MTLINSP_HOTKEY is worth knowing about there.
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <type_traits>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#import <AppKit/AppKit.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace gpuhud
{

// What was bound. Function keys and single characters cover everything worth binding here; the
// three lock/system keys are in because PrintScreen is what RenderDoc trained everyone to press.
enum class HotkeyKey : int
{
    None = 0,
    F1 = 1,   // .. F12 = 12, so F(n) is simply (int)HotkeyKey::F1 + n - 1
    F12 = 12,
    PrintScreen = 20,
    ScrollLock = 21,
    Pause = 22,
    Character = 30,   // `ch` holds it: 'A'..'Z' or '0'..'9'
};

struct HotkeyBinding
{
    HotkeyKey key = HotkeyKey::None;
    char ch = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    char name[32] = {};   // canonical and uppercase ("CTRL+F11"): what the HUD prints

    bool bound() const { return key != HotkeyKey::None; }
};

/**
 * "F11", "f9", "ctrl+F11", "PrintScreen", "A". Empty means the default, F11; "off", "none", "0"
 * and "" -- anything that is deliberately nothing -- clear the binding. Returns false when the
 * name means nothing here, in which case `out` is left unbound and the caller logs it: a
 * misspelled setting should say so rather than quietly leave the user pressing a dead key.
 */
inline bool ParseHotkey(const char* text, HotkeyBinding& out)
{
    out = HotkeyBinding{};
    char buffer[64];
    const size_t len = text ? strnlen(text, sizeof(buffer) - 1) : 0;
    for (size_t i = 0; i < len; ++i)
    {
        const char c = text[i];
        buffer[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    buffer[len] = 0;
    if (len == 0)
        strcpy(buffer, "F11");
    if (strcmp(buffer, "OFF") == 0 || strcmp(buffer, "NONE") == 0 || strcmp(buffer, "0") == 0)
        return true;   // no binding, and nothing wrong with that

    // The modifiers, then whatever is left is the key itself.
    const char* p = buffer;
    for (;;)
    {
        const char* plus = strchr(p, '+');
        if (!plus)
            break;
        const size_t n = (size_t)(plus - p);
        if (n == 4 && strncmp(p, "CTRL", 4) == 0)
            out.ctrl = true;
        else if (n == 7 && strncmp(p, "CONTROL", 7) == 0)
            out.ctrl = true;
        else if (n == 5 && strncmp(p, "SHIFT", 5) == 0)
            out.shift = true;
        else if (n == 3 && strncmp(p, "ALT", 3) == 0)
            out.alt = true;
        else if (n == 6 && strncmp(p, "OPTION", 6) == 0)
            out.alt = true;
        else
            return false;
        p = plus + 1;
    }

    if (p[0] == 'F' && p[1] >= '1' && p[1] <= '9')
    {
        const int n = (p[2] >= '0' && p[2] <= '9') ? (p[1] - '0') * 10 + (p[2] - '0') : (p[1] - '0');
        const size_t digits = (p[2] >= '0' && p[2] <= '9') ? 2u : 1u;
        if (p[1 + digits] != 0 || n < 1 || n > 12)
            return false;
        out.key = (HotkeyKey)((int)HotkeyKey::F1 + n - 1);
    }
    else if (strcmp(p, "PRINTSCREEN") == 0 || strcmp(p, "PRTSC") == 0 || strcmp(p, "PRINT") == 0)
        out.key = HotkeyKey::PrintScreen;
    else if (strcmp(p, "SCROLLLOCK") == 0 || strcmp(p, "SCROLL_LOCK") == 0)
        out.key = HotkeyKey::ScrollLock;
    else if (strcmp(p, "PAUSE") == 0)
        out.key = HotkeyKey::Pause;
    else if (p[1] == 0 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= '0' && p[0] <= '9')))
    {
        out.key = HotkeyKey::Character;
        out.ch = p[0];
    }
    else
        return false;

    snprintf(out.name, sizeof(out.name), "%s%s%s%s", out.ctrl ? "CTRL+" : "", out.shift ? "SHIFT+" : "",
        out.alt ? "ALT+" : "", p);
    return true;
}

// -------------------------------------------------------------------------------------------
// Windows

#if defined(_WIN32)

/** The virtual-key code, or 0 for a binding this platform has no key for. */
inline int HotkeyVirtualKey(const HotkeyBinding& b)
{
    const int n = (int)b.key;
    if (n >= (int)HotkeyKey::F1 && n <= (int)HotkeyKey::F12)
        return VK_F1 + (n - (int)HotkeyKey::F1);
    switch (b.key)
    {
        // Windows keeps PrintScreen for itself (and Steam takes it in a game): the key press may
        // never reach GetAsyncKeyState at all, which is why the documentation does not offer it.
        case HotkeyKey::PrintScreen: return VK_SNAPSHOT;
        case HotkeyKey::ScrollLock: return VK_SCROLL;
        case HotkeyKey::Pause: return VK_PAUSE;
        // 'A'..'Z' and '0'..'9' are their own virtual-key codes.
        case HotkeyKey::Character: return (int)(unsigned char)b.ch;
        default: return 0;
    }
}

/**
 * Whether the key is held *and* this process owns the foreground window. Without the second half
 * the key would be global: pressing it in the inspector's own window, or in any other application
 * on the machine, would take a capture of a target that is not even being looked at.
 */
inline bool HotkeyHeld(const HotkeyBinding& b)
{
    const int vk = HotkeyVirtualKey(b);
    if (!vk)
        return false;
    const HWND foreground = GetForegroundWindow();
    if (!foreground)
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    if (pid != GetCurrentProcessId())
        return false;
    auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    if (!down(vk))
        return false;
    // Exactly the modifiers asked for, so CTRL+F11 does not also fire the F11 binding.
    return down(VK_CONTROL) == b.ctrl && down(VK_SHIFT) == b.shift && down(VK_MENU) == b.alt;
}

inline bool HotkeySupported(const HotkeyBinding& b) { return HotkeyVirtualKey(b) != 0; }

// -------------------------------------------------------------------------------------------
// Linux (X11)

#elif defined(__linux__) && !defined(__ANDROID__)

/**
 * The handful of Xlib entry points this needs, loaded once. Declared here with `void*` and
 * `unsigned long` rather than by including Xlib.h, which would drag its `None`, `Status` and
 * `Bool` macros into every backend that includes this header -- the same clash refresh_rate.h
 * already has to warn about.
 */
struct HotkeyXlib
{
    void* lib = nullptr;
    void* (*OpenDisplay)(const char*) = nullptr;
    int (*CloseDisplay)(void*) = nullptr;
    int (*QueryKeymap)(void*, char*) = nullptr;
    unsigned long (*StringToKeysym)(const char*) = nullptr;
    unsigned char (*KeysymToKeycode)(void*, unsigned long) = nullptr;
    unsigned long (*InternAtom)(void*, const char*, int) = nullptr;
    unsigned long (*DefaultRootWindowFn)(void*) = nullptr;
    int (*GetWindowProperty)(void*, unsigned long, unsigned long, long, long, int, unsigned long,
        unsigned long*, int*, unsigned long*, unsigned long*, unsigned char**) = nullptr;
    int (*FreeFn)(void*) = nullptr;
    void* display = nullptr;
    bool ok = false;
};

inline HotkeyXlib& HotkeyX11()
{
    static HotkeyXlib x = [] {
        HotkeyXlib r;
        // No DISPLAY is a console, a Wayland-native session or a headless run: no keyboard to ask
        // about, and no hotkey.
        if (!getenv("DISPLAY"))
            return r;
        r.lib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
        if (!r.lib)
            return r;
        bool bound = true;
        auto bind = [&r, &bound](const char* name, auto& fn) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)> >(dlsym(r.lib, name));
            if (!fn)
                bound = false;
        };
        bind("XOpenDisplay", r.OpenDisplay);
        bind("XCloseDisplay", r.CloseDisplay);
        bind("XQueryKeymap", r.QueryKeymap);
        bind("XStringToKeysym", r.StringToKeysym);
        bind("XKeysymToKeycode", r.KeysymToKeycode);
        bind("XInternAtom", r.InternAtom);
        bind("XDefaultRootWindow", r.DefaultRootWindowFn);
        bind("XGetWindowProperty", r.GetWindowProperty);
        bind("XFree", r.FreeFn);
        if (!bound)
            return r;
        // A connection of the library's own: the application's Display belongs to its own thread
        // and must not be borrowed, and this one is only ever used under the poll's lock.
        r.display = r.OpenDisplay(nullptr);
        r.ok = r.display != nullptr;
        return r;
    }();
    return x;
}

/** The X keysym name for a binding, or nullptr when there is no such key on X11. */
inline const char* HotkeyKeysymName(const HotkeyBinding& b, char (&storage)[8])
{
    const int n = (int)b.key;
    if (n >= (int)HotkeyKey::F1 && n <= (int)HotkeyKey::F12)
    {
        snprintf(storage, sizeof(storage), "F%d", n - (int)HotkeyKey::F1 + 1);
        return storage;
    }
    switch (b.key)
    {
        case HotkeyKey::PrintScreen: return "Print";
        case HotkeyKey::ScrollLock: return "Scroll_Lock";
        case HotkeyKey::Pause: return "Pause";
        case HotkeyKey::Character:
            // Letters are bound by their unshifted keysym, which is the lowercase one.
            storage[0] = (b.ch >= 'A' && b.ch <= 'Z') ? (char)(b.ch - 'A' + 'a') : b.ch;
            storage[1] = 0;
            return storage;
        default: return nullptr;
    }
}

inline bool HotkeySupported(const HotkeyBinding& b)
{
    char storage[8];
    return b.bound() && HotkeyKeysymName(b, storage) != nullptr && HotkeyX11().ok;
}

/**
 * Whether the window the window manager calls active belongs to this process.
 *
 * _NET_ACTIVE_WINDOW and _NET_WM_PID are EWMH, which every desktop window manager in use sets and
 * a bare window manager may not. When they are missing there is nothing to test against, and the
 * choice is between a hotkey that never works and one that is not focus-aware; this takes the
 * second, since the key is only ever live while the user has switched the HUD on for this session.
 */
inline bool HotkeyWindowFocused(HotkeyXlib& x)
{
    const unsigned long active = x.InternAtom(x.display, "_NET_ACTIVE_WINDOW", 1 /*only if it exists*/);
    const unsigned long pidAtom = x.InternAtom(x.display, "_NET_WM_PID", 1);
    if (!active || !pidAtom)
        return true;
    unsigned long type = 0, count = 0, after = 0;
    int format = 0;
    unsigned char* data = nullptr;
    if (x.GetWindowProperty(x.display, x.DefaultRootWindowFn(x.display), active, 0, 1, 0 /*delete*/,
            0 /*AnyPropertyType*/, &type, &format, &count, &after, &data) != 0 /*Success*/
        || !data)
        return true;
    const unsigned long window = (format == 32 && count >= 1) ? *reinterpret_cast<const unsigned long*>(data) : 0;
    x.FreeFn(data);
    if (!window)
        return false;
    data = nullptr;
    if (x.GetWindowProperty(x.display, window, pidAtom, 0, 1, 0, 0,
            &type, &format, &count, &after, &data) != 0 ||
        !data)
        return true;
    const bool mine = format == 32 && count >= 1 &&
        *reinterpret_cast<const unsigned long*>(data) == (unsigned long)getpid();
    x.FreeFn(data);
    return mine;
}

inline bool HotkeyHeld(const HotkeyBinding& b)
{
    HotkeyXlib& x = HotkeyX11();
    if (!x.ok)
        return false;
    char storage[8];
    const char* name = HotkeyKeysymName(b, storage);
    if (!name)
        return false;
    const unsigned long keysym = x.StringToKeysym(name);
    if (!keysym)
        return false;
    char keys[32] = {};
    if (!x.QueryKeymap(x.display, keys))
        return false;
    auto down = [&keys, &x](unsigned long sym) {
        const unsigned char code = x.KeysymToKeycode(x.display, sym);
        return code != 0 && (keys[code >> 3] & (1 << (code & 7))) != 0;
    };
    if (!down(keysym))
        return false;
    if (!HotkeyWindowFocused(x))
        return false;
    // Exactly the modifiers asked for, either side of the keyboard.
    const bool ctrl = down(x.StringToKeysym("Control_L")) || down(x.StringToKeysym("Control_R"));
    const bool shift = down(x.StringToKeysym("Shift_L")) || down(x.StringToKeysym("Shift_R"));
    const bool alt = down(x.StringToKeysym("Alt_L")) || down(x.StringToKeysym("Alt_R"));
    return ctrl == b.ctrl && shift == b.shift && alt == b.alt;
}

// -------------------------------------------------------------------------------------------
// macOS
//
// The monitor below reads whatever the key event carries, so anything a Mac keyboard has is
// readable; PrintScreen, Scroll Lock and Pause are not among them.

#elif defined(__APPLE__)

inline bool HotkeySupported(const HotkeyBinding& b)
{
    const int n = (int)b.key;
    return (n >= (int)HotkeyKey::F1 && n <= (int)HotkeyKey::F12) || b.key == HotkeyKey::Character;
}

// -------------------------------------------------------------------------------------------
// Everything else (Android, and a Linux build without X11)

#else

inline bool HotkeySupported(const HotkeyBinding&) { return false; }
inline bool HotkeyHeld(const HotkeyBinding&) { return false; }

#endif

// -------------------------------------------------------------------------------------------

/**
 * The hotkey itself: the binding, whether it is armed, and one edge-detected press per present.
 *
 * Polled from the frame loop rather than acted on the moment the key goes down, so a capture is
 * always asked for at a frame boundary, on a thread the backend already owns -- and so the press
 * of a key held down for half a second is one capture rather than thirty.
 */
class CaptureHotkey
{
public:
    static CaptureHotkey& Get()
    {
        static CaptureHotkey* instance = new CaptureHotkey();
        return *instance;
    }

    /**
     * From the backend's *_HOTKEY setting at start-up. False when the name means nothing, which
     * the backend logs; the binding is then left off rather than silently falling back to F11,
     * since a user who asked for a key deserves to be told they did not get it.
     */
    bool SetBinding(const char* name)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        HotkeyBinding parsed;
        const bool ok = ParseHotkey(name, parsed);
        _binding = ok ? parsed : HotkeyBinding{};
        _held = false;
#if defined(__APPLE__)
        // The monitor carries a copy of the binding it was made with, so a rebinding while it is
        // installed has to make it again. (The settings are read before the HUD is ever armed, so
        // in practice there is nothing installed yet.)
        if (_enabled)
        {
            RemoveMonitor();
            InstallMonitor();
        }
#endif
        return ok;
    }

    /** Armed with the HUD, which is what says on the screen that the key is live. */
    void SetEnabled(bool on)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_enabled == on)
            return;
        _enabled = on;
        // A key held while the HUD was switched on must not read as a press on the first poll.
        _held = true;
        _pressed.store(false, std::memory_order_relaxed);
#if defined(__APPLE__)
        if (on)
            InstallMonitor();
        else
            RemoveMonitor();
#endif
    }

    /**
     * The key's name for the HUD ("F11"), or nullptr when there is no usable hotkey -- no binding,
     * a key this platform does not have, or no way to read the keyboard at all.
     */
    const char* Name()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_enabled || !_binding.bound() || !HotkeySupported(_binding))
            return nullptr;
        return _binding.name;
    }

    /** True once per press. Called once a present, from whichever thread is presenting. */
    bool Poll()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_enabled || !_binding.bound())
            return false;
#if defined(__APPLE__)
        // The monitor has already done the edge detection: a key event is a press.
        return _pressed.exchange(false, std::memory_order_relaxed);
#else
        const bool held = HotkeyHeld(_binding);
        const bool pressed = held && !_held;
        _held = held;
        return pressed;
#endif
    }

private:
#if defined(__APPLE__)
    /**
     * A *local* monitor: it sees only the key events this application is being sent, which is the
     * focus rule the other platforms have to reconstruct from the foreground window, and it needs
     * no accessibility permission -- a global monitor or a CGEventTap would ask the user for one
     * the first time the HUD was switched on. The event is returned unchanged, so the application
     * still gets the key.
     *
     * Installed on the main thread, where AppKit's event machinery lives; the HUD is switched on
     * from the transport's thread.
     */
    void InstallMonitor()
    {
        // The binding is copied into the block rather than read through `this`: the handler runs on
        // the main thread, and _binding is only ever touched under the lock held right now.
        HotkeyBinding binding = _binding;
        std::atomic<bool>* pressed = &_pressed;
        OnMainThread(^{
          if (_monitor != nil)
              return;
          _monitor = [[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                            handler:^NSEvent*(NSEvent* event) {
                                                              if (MatchesEvent(binding, event))
                                                                  pressed->store(true, std::memory_order_relaxed);
                                                              return event;
                                                            }] retain];
        });
    }

    void RemoveMonitor()
    {
        OnMainThread(^{
          if (_monitor == nil)
              return;
          [NSEvent removeMonitor:_monitor];
          [_monitor release];
          _monitor = nil;
        });
    }

    /**
     * Runs the block on the main thread, now if that is where we are. Everything that touches
     * `_monitor` goes through here, so the two blocks above never run against each other however
     * the HUD is switched.
     */
    static void OnMainThread(dispatch_block_t block)
    {
        if ([NSThread isMainThread])
            block();
        else
            dispatch_async(dispatch_get_main_queue(), block);
    }

    /** macOS virtual key codes for F1..F12, which are not consecutive. */
    static unsigned short FunctionKeyCode(int n)
    {
        static const unsigned short codes[12] = {
            0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F};
        return (n >= 1 && n <= 12) ? codes[n - 1] : 0;
    }

    static bool MatchesEvent(const HotkeyBinding& b, NSEvent* event)
    {
        const NSEventModifierFlags flags = [event modifierFlags];
        // Command is never part of a binding, and Fn is set simply by pressing a function key on
        // a Mac keyboard, so neither is compared; caps lock is ignored for the same reason.
        if (((flags & NSEventModifierFlagControl) != 0) != b.ctrl)
            return false;
        if (((flags & NSEventModifierFlagShift) != 0) != b.shift)
            return false;
        if (((flags & NSEventModifierFlagOption) != 0) != b.alt)
            return false;
        const int n = (int)b.key;
        if (n >= (int)HotkeyKey::F1 && n <= (int)HotkeyKey::F12)
            return [event keyCode] == FunctionKeyCode(n - (int)HotkeyKey::F1 + 1);
        if (b.key == HotkeyKey::Character)
        {
            NSString* characters = [[event charactersIgnoringModifiers] uppercaseString];
            return [characters length] == 1 && [characters characterAtIndex:0] == (unichar)b.ch;
        }
        return false;   // no PrintScreen, Scroll Lock or Pause on a Mac keyboard
    }

    id _monitor = nil;
#endif

    std::mutex _mutex;
    HotkeyBinding _binding;
    bool _enabled = false;
    bool _held = false;                    // the key's state at the previous poll
    std::atomic<bool> _pressed{false};     // macOS: set by the monitor, consumed by the poll
};

}   // namespace gpuhud
