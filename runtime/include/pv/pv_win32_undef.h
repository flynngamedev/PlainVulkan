// pv_win32_undef.h -- undo the Win32 A/W preprocessor macros that collide
// with PlainVulkan command names.
//
// <windows.h> (dragged in transitively by GLFW, Vulkan's
// VK_USE_PLATFORM_WIN32_KHR path, miniaudio's WASAPI backend, PhysX, etc.)
// #defines a family of names as macros -- CreateWindow, DrawText, PlaySound,
// DeleteFile and friends expand to their ...W/...A variants depending on
// UNICODE. Several of these exactly match functions PlainVulkan declares as
// pv::rt::Foo(...). Because the preprocessor is purely textual, the macro
// mangles BOTH the declaration in pv_runtime.h AND every generated call site
// (pv::rt::DrawText(...) still expands -- :: scoping means nothing to the
// preprocessor). NOGDI / WIN32_LEAN_AND_MEAN do NOT help here: these macros
// live in winuser.h (not wingdi.h), and winuser.h can't be excluded because
// GLFW needs it.
//
// The only robust fix is to #undef each colliding name. This header is
// included:
//   - late in pv_platform.h  (covers every runtime .cpp)
//   - near the top of pv_runtime.h (covers generated game code, which
//     includes pv_runtime.h but not pv_platform.h)
// so that by the time any pv::rt::Foo declaration or call is seen, the macro
// is gone. Safe to include repeatedly (#undef of an undefined macro is a
// no-op) and safe on non-Windows (the names simply aren't macros there).
#pragma once

#if defined(_WIN32)
#  ifdef CreateWindow
#    undef CreateWindow
#  endif
#  ifdef DrawText
#    undef DrawText
#  endif
#  ifdef DrawTextEx
#    undef DrawTextEx
#  endif
#  ifdef PlaySound
#    undef PlaySound
#  endif
#  ifdef DeleteFile
#    undef DeleteFile
#  endif
#endif
