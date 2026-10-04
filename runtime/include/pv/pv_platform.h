// pv_platform.h -- the one place PlainVulkan deals with per-OS and
// per-compiler differences. Included first by pv_internal.h (and by any
// runtime .cpp that doesn't include pv_internal.h), so nothing else in
// the codebase needs an #ifdef _WIN32.
//
// Why this file exists: before native Windows support, the runtime leaned
// on a handful of things that are POSIX-only or GCC/Clang-only in
// practice -- <unistd.h>, readlink("/proc/self/exe"), M_PI from <cmath>,
// and unqualified std::min/std::max. On MSVC every one of those either
// fails to compile or (worse) compiles into something subtly wrong, so
// they're all handled here once.
#pragma once

// ---------------------------------------------------------------------
// Windows.h hygiene. These MUST be defined before any header that might
// transitively pull in <windows.h> -- which on Windows includes
// GLFW/glfw3.h, vulkan.h (via VK_USE_PLATFORM_WIN32_KHR), and PhysX.
//
//   NOMINMAX          -- otherwise windows.h #defines min/max as macros,
//                        and every `std::min(a, b)` in the runtime becomes
//                        a syntax error ("'(': illegal token on right side
//                        of '::'"). This is the single most common cause
//                        of a Unix-developed C++ codebase failing to build
//                        on MSVC.
//   WIN32_LEAN_AND_MEAN -- drops winsock/OLE/RPC from windows.h. Cuts
//                        compile time substantially and avoids a symbol
//                        clash between winsock.h and winsock2.h if
//                        anything downstream wants sockets.
//   NOGDI             -- windows.h's GDI section (wingdi.h) defines
//                        `Rectangle`, `Polygon`, etc. as macros. NOTE: it
//                        does NOT define `CreateWindow` -- that macro lives
//                        in winuser.h, which can't be excluded (GLFW needs
//                        it). The CreateWindow/DrawText/PlaySound/DeleteFile
//                        collisions are undone explicitly via
//                        pv/pv_win32_undef.h instead; see that header.
// ---------------------------------------------------------------------
#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX 1
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  ifndef NOGDI
#    define NOGDI 1
#  endif
#  ifndef UNICODE
#    define UNICODE 1
#  endif
#  ifndef _UNICODE
#    define _UNICODE 1
#  endif
// Silences C4996 on std::getenv, fopen, etc. We use the standard forms
// deliberately (they're portable); MSVC's _s variants are not.
#  ifndef _CRT_SECURE_NO_WARNINGS
#    define _CRT_SECURE_NO_WARNINGS 1
#  endif
#endif

// ---------------------------------------------------------------------
// Math constants. M_PI is POSIX, not ISO C++ -- MSVC only defines it if
// _USE_MATH_DEFINES is set before <cmath>, and even then it's a macro
// rather than a typed constant. The runtime uses PV_PI everywhere
// instead, which is well-defined on every compiler.
// ---------------------------------------------------------------------
// The !defined(M_PI) guard matters: several vendored single-header
// libraries (stb_vorbis.c is the one that bites, from audio.cpp) define
// M_PI themselves before this header is reached. Turning on
// _USE_MATH_DEFINES after that point makes <cmath> pull in
// corecrt_math_defines.h, which defines M_PI a second time -- MSVC C4005,
// and a hard error for anyone building with /WX. Nothing in this codebase
// reads M_PI (PV_PI below is what the runtime uses), so if some other
// header already provided it, leave it alone.
#if defined(_WIN32) && !defined(_USE_MATH_DEFINES) && !defined(M_PI)
#  define _USE_MATH_DEFINES 1
#endif

#include <cmath>
#include <cstdint>
#include <string>

namespace pv {

constexpr float PV_PI = 3.14159265358979323846f;
constexpr float PV_TAU = 6.28318530717958647692f;
constexpr float PV_DEG2RAD = PV_PI / 180.0f;
constexpr float PV_RAD2DEG = 180.0f / PV_PI;

// ---------------------------------------------------------------------
// Path handling. Windows accepts '/' in nearly every API, so the runtime
// normalizes *toward* '/' internally and only converts at the boundary.
// The native separator matters for the two places we build paths that get
// shown to a user (log messages, the editor's asset browser).
// ---------------------------------------------------------------------
#if defined(_WIN32)
constexpr char PV_PATH_SEP = '\\';
constexpr const char* PV_EXE_SUFFIX = ".exe";
#else
constexpr char PV_PATH_SEP = '/';
constexpr const char* PV_EXE_SUFFIX = "";
#endif

// Replaces backslashes with forward slashes. Safe to call on any path;
// used before handing a path to CMake, to cgltf (which does its own
// relative-URI resolution and only understands '/'), and before writing
// a path into a .pvscene file so scenes stay portable between OSes.
std::string normalizeSlashes(std::string path);

// Joins two path fragments with a single '/', tolerating a trailing
// separator on `a` and a leading one on `b`.
std::string joinPath(const std::string& a, const std::string& b);

// Absolute path of the directory containing the running executable.
// Implemented per-OS in vk_core.cpp:
//   Windows -- GetModuleFileNameW + wide->UTF-8 conversion
//   macOS   -- _NSGetExecutablePath
//   Linux   -- readlink("/proc/self/exe")
// Used to locate `<exe_dir>/shaders/*.spv` regardless of the working
// directory the game was launched from (double-clicking a .exe on Windows
// sets cwd to the user's home or the Desktop, not the exe's folder, so
// relative shader paths would otherwise break -- this is why the function
// exists at all).
std::string exeDirectory();

// UTF-8 <-> UTF-16 conversion, needed only on Windows where the "W" API
// family takes wchar_t. No-ops elsewhere but still defined, so calling
// code doesn't need an #ifdef.
#if defined(_WIN32)
std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);
#endif

// Opens a console window and reattaches stdout/stderr to it. On Windows a
// GUI-subsystem binary (which is what a game should be, so launching it
// doesn't flash a console) has no stdout at all, meaning every Pv::Log
// call would silently vanish. Called by Pv::Init when PV_CONSOLE=1 is set
// in the environment, and unconditionally by the editor.
void ensureConsole(bool allocIfNoParent = true);

} // namespace pv

// ---------------------------------------------------------------------
// Symbol visibility. The runtime builds as a static library today, but
// the editor loads game logic and the plan is to allow a shared-library
// build later; getting the annotation in now costs nothing.
// ---------------------------------------------------------------------
#if defined(_WIN32)
#  define PV_EXPORT __declspec(dllexport)
#  define PV_IMPORT __declspec(dllimport)
#else
#  define PV_EXPORT __attribute__((visibility("default")))
#  define PV_IMPORT
#endif

// MSVC doesn't support __attribute__((unused)); [[maybe_unused]] is C++17
// and works everywhere the runtime targets.
#define PV_UNUSED [[maybe_unused]]
