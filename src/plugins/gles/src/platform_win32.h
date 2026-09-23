// Windows: hooking the libraries OpenGL ES comes from (platform_win32.cpp), shared with the WGL hooks.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace glesinsp
{

/** Patches `name` in `module` to jump to `hook`; `real` gets the trampoline. False when the module lacks it. */
bool HookExport(HMODULE module, const char* name, void* hook, void** real);
/** Turns on the hooks made since the last call. */
void EnableHooks();
/** A library exporting GL entry points (libGLESv2.dll, opengl32.dll): the recorded ones hooked, the rest looked up. */
void HookGlModule(HMODULE module);
/** opengl32.dll's WGL entry points (hooks_wgl.cpp). */
void HookWgl(HMODULE opengl32);
/** gdi32.dll's SwapBuffers, where a WGL application's frames end (hooks_wgl.cpp). */
void HookGdiSwap(HMODULE gdi32);

}  // namespace glesinsp
