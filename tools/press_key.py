"""Presses a key in another process's window, so a hotkey can be tested without a human at it.

    python tools/press_key.py <image.exe> [delay-seconds] [key]

Waits `delay-seconds`, finds the first visible window of a process with that image name, brings it
to the front and synthesizes one press and release of `key` (F11 by default). Used by
tools/ui_tests.py as a case's companion process, for the HUD's capture hotkey
(src/vulkan/src/hud_hotkey.h): the key is only live while the application it was pressed in owns
the foreground window, so a test that simply sent the key nowhere in particular would prove
nothing. It then stays alive until it is killed, which is how the harness ends a companion.

Windows only: the capture libraries read the keyboard through each platform's own API, and this
reproduces a real key press only on the one where the tests run.
"""
import ctypes
import ctypes.wintypes as wintypes
import subprocess
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

# The keys worth pressing here; the virtual-key codes the capture libraries look for.
KEYS = {f"F{n}": 0x70 + n - 1 for n in range(1, 13)}
KEYEVENTF_KEYUP = 0x0002


def pids_for(image):
    """The process ids running that image, from tasklist (no extra module to install)."""
    text = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image}", "/FO", "CSV", "/NH"],
                          capture_output=True, text=True).stdout
    pids = set()
    for line in text.splitlines():
        fields = [f.strip('"') for f in line.split('","')]
        if len(fields) > 1 and fields[0].lower() == image.lower():
            pids.add(int(fields[1]))
    return pids


def window_of(pids):
    """The first visible top-level window belonging to one of those processes."""
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum(hwnd, _):
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in pids and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(enum, 0)
    return found[0] if found else None


def bring_to_front(hwnd):
    """
    Windows refuses SetForegroundWindow from a process that does not own the foreground window, so
    this attaches to both input queues first -- what every alt-tab helper does -- and only then asks.
    """
    foreground = user32.GetForegroundWindow()
    ours = kernel32.GetCurrentThreadId()
    theirs = user32.GetWindowThreadProcessId(hwnd, None)
    current = user32.GetWindowThreadProcessId(foreground, None)
    user32.AttachThreadInput(ours, current, True)
    user32.AttachThreadInput(ours, theirs, True)
    user32.SetForegroundWindow(hwnd)
    user32.SetActiveWindow(hwnd)
    user32.AttachThreadInput(ours, theirs, False)
    user32.AttachThreadInput(ours, current, False)
    time.sleep(0.5)
    return user32.GetForegroundWindow() == hwnd


def main():
    if sys.platform != "win32":
        print("press_key.py is Windows only", file=sys.stderr)
        return 2
    image = sys.argv[1] if len(sys.argv) > 1 else ""
    delay = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
    key = (sys.argv[3] if len(sys.argv) > 3 else "F11").upper()
    if not image or key not in KEYS:
        print(__doc__, file=sys.stderr)
        return 2
    time.sleep(delay)
    pids = pids_for(image)
    window = window_of(pids) if pids else None
    if not window:
        print(f"no visible window for {image}", flush=True)
        return 1
    if not bring_to_front(window):
        print(f"{image} did not come to the front", flush=True)
        return 1
    user32.keybd_event(KEYS[key], 0, 0, 0)
    time.sleep(0.15)
    user32.keybd_event(KEYS[key], 0, KEYEVENTF_KEYUP, 0)
    print(f"{key} pressed in {image}", flush=True)
    # The harness kills its companion when the case ends; until then there is nothing left to do.
    time.sleep(300)
    return 0


if __name__ == "__main__":
    sys.exit(main())
