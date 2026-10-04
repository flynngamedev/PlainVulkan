#!/usr/bin/env bash
#
# verify_msvc.sh -- build and test PlainVulkan with the real Microsoft C++
# compiler, from Linux, using msvc-wine.
#
# cl.exe and link.exe here are Microsoft's own binaries running under Wine,
# producing PE objects against the Microsoft CRT and the Windows SDK -- the
# same compiler a Developer Command Prompt invokes. Wine substitutes for the
# operating system underneath, not for the toolchain.
#
# ---------------------------------------------------------------------
# One-time setup
# ---------------------------------------------------------------------
#   sudo apt-get install -y wine64 msitools winbind cmake ninja-build glslang-tools
#   git clone https://github.com/mstorsjo/msvc-wine && cd msvc-wine
#   ./vsdownload.py --accept-license --dest /opt/msvc \
#       --architecture x64 --host-arch x64 --only-host yes \
#       --with-default no --with-workload no --with-atl no --with-dia no \
#       --with-msbuild no --with-devcmd no --with-asan no \
#       --with-msvc yes --with-sdk yes
#   ./install.sh /opt/msvc
#
# PlainVulkan additionally needs Windows-side Vulkan headers and an import
# library. There is no need to install the Windows Vulkan SDK under Wine --
# the headers are public and the import library can be generated from the
# loader's own .def file with MSVC's lib.exe:
#
#   mkdir -p /opt/vulkan-sdk/Include /opt/vulkan-sdk/Lib
#   git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Headers /tmp/vh
#   git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Loader  /tmp/vl
#   cp -r /tmp/vh/include/vulkan /tmp/vh/include/vk_video /opt/vulkan-sdk/Include/
#   grep -v '^;' /tmp/vl/loader/vulkan-1.def | sed 's/@VULKAN_1_[A-Z_]*@//' > /tmp/vulkan-1.def
#   PATH=/opt/msvc/bin/x64:$PATH lib /def:/tmp/vulkan-1.def \
#       /out:/opt/vulkan-sdk/Lib/vulkan-1.lib /machine:x64
#
# GLFW needs no setup: the build fetches and compiles it with the same
# toolchain. Shaders are compiled by the host's glslangValidator, which is
# correct -- SPIR-V is platform independent.
#
# Downloading MSVC this way is subject to the Visual Studio licence you
# accept with --accept-license. Read it; it governs what you may do with
# the resulting toolchain.
#
# Usage:  bash scripts/verify_msvc.sh
#         MSVC_ROOT=/opt/msvc VULKAN_SDK=/opt/vulkan-sdk bash scripts/verify_msvc.sh
#
set -uo pipefail

MSVC_ROOT=${MSVC_ROOT:-/opt/msvc}
VKSDK=${VULKAN_SDK:-/opt/vulkan-sdk}
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK=${WORK:-/tmp/plainvulkan_msvc}

RED=$'\033[31m'; GREEN=$'\033[32m'; BOLD=$'\033[1m'; OFF=$'\033[0m'
PASSED=0; FAILED=0; FAILED_NAMES=()

pass() { PASSED=$((PASSED+1)); printf '  %sPASS%s  %s\n' "$GREEN" "$OFF" "$1"; }
fail() { FAILED=$((FAILED+1)); FAILED_NAMES+=("$1"); printf '  %sFAIL%s  %s\n' "$RED" "$OFF" "$1"
         [ -n "${2:-}" ] && sed 's/^/        | /' <<< "$2"; }
section() { printf '\n%s== %s ==%s\n' "$BOLD" "$1" "$OFF"; }

if [ ! -x "$MSVC_ROOT/bin/x64/cl" ]; then
  echo "msvc-wine not found at $MSVC_ROOT. See the setup notes at the top of this script."
  exit 2
fi
if [ ! -f "$VKSDK/Lib/vulkan-1.lib" ]; then
  echo "Windows Vulkan import library not found at $VKSDK/Lib/vulkan-1.lib."
  echo "See the setup notes at the top of this script."
  exit 2
fi

export PATH="$MSVC_ROOT/bin/x64:$PATH"
export WINEDEBUG=${WINEDEBUG:--all}
mkdir -p "$WORK"

CM_COMMON=(-G Ninja -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=AMD64
           -DCMAKE_CXX_COMPILER=cl -DCMAKE_C_COMPILER=cl -DCMAKE_RC_COMPILER=rc
           -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded
           -DCMAKE_POLICY_DEFAULT_CMP0141=NEW
           -DVulkan_INCLUDE_DIR="$VKSDK/Include"
           -DVulkan_LIBRARY="$VKSDK/Lib/vulkan-1.lib"
           -DCMAKE_BUILD_TYPE=Release)

section "0. Toolchain identity"
CLVER=$(cl 2>&1 | grep -o 'Version [0-9.]*' | head -1)
if [ -n "$CLVER" ]; then pass "cl.exe is Microsoft's ($CLVER)"; else fail "cl.exe did not report a version"; fi

build_target() { # <label> <source dir> <workdir>
  local label=$1 src=$2 wd=$3
  rm -rf "$wd" && mkdir -p "$wd"
  if cmake -S "$src" -B "$wd" "${CM_COMMON[@]}" -DPV_WITH_PHYSX=OFF > "$wd.cfg.log" 2>&1; then
    pass "$label: cmake configure"
  else
    fail "$label: cmake configure" "$(tail -15 "$wd.cfg.log")"; return
  fi
  if ( cd "$wd" && ninja ) > "$wd.build.log" 2>&1; then
    pass "$label: build"
  else
    fail "$label: build" "$(grep -E 'error C|fatal error|LNK[0-9]{4}' "$wd.build.log" | head -10)"; return
  fi
  # Warnings from fetched third-party sources (_deps) are not ours to fix;
  # anything from this codebase is.
  local w
  w=$(grep 'warning C' "$wd.build.log" | grep -v '_deps' | head -8)
  if [ -z "$w" ]; then pass "$label: no MSVC warnings from project code"; else
    fail "$label: MSVC warnings from project code" "$w"; fi
}

section "1. Game binary (examples/mini_game)"
build_target "mini_game" "$ROOT/examples/mini_game/build" "$WORK/game"

section "2. Terrain + AI example (examples/terrain_ai)"
# Built separately from mini_game because it is the one example that
# exercises terrain.cpp, terrain_render.cpp and ai.cpp end to end -- if
# those only ever compiled as part of the library, a codegen or linkage
# problem in the new commands would not show up here.
if [ -d "$ROOT/examples/terrain_ai/build" ]; then
  build_target "terrain_ai" "$ROOT/examples/terrain_ai/build" "$WORK/terrain"
else
  printf '  %sSKIP%s  terrain_ai (run `pv build examples/terrain_ai` once first)\n' $'\033[33m' "$OFF"
fi

section "3. Scene editor (pv edit)"
build_target "pvedit" "$ROOT/editor-build" "$WORK/editor"

section "4. Test suites -- compile with MSVC, run under Wine"
mkdir -p "$WORK/tests" && cd "$WORK/tests"
if cl /nologo /std:c++17 /EHsc /permissive- /Zc:__cplusplus /utf-8 /O2 /W3 \
     /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DRVO_STATIC_DEFINE \
     /I"$ROOT/runtime/include" /I"$VKSDK/Include" \
     /I"$ROOT/runtime/third_party/Ai/RVO2/src" \
     /I"$ROOT/runtime/third_party/Ai/recastnavigation/Recast/Include" \
     /I"$ROOT/runtime/third_party/Ai/recastnavigation/Detour/Include" \
     "$ROOT/runtime/tests/runtime_tests.cpp" "$ROOT/runtime/src/pv_math.cpp" \
     "$ROOT/runtime/src/pv_json.cpp" "$ROOT/runtime/src/anim_sample.cpp" \
     "$ROOT/runtime/src/pv_value.cpp" "$ROOT/runtime/src/terrain.cpp" \
     "$ROOT/runtime/src/ai.cpp" "$ROOT/runtime/src/ai_navmesh.cpp" \
     "$ROOT/runtime/third_party/Ai/recastnavigation/Recast/Source/"*.cpp \
     "$ROOT/runtime/third_party/Ai/recastnavigation/Detour/Source/"*.cpp \
     "$ROOT/runtime/third_party/Ai/RVO2/src/"*.cc \
     /Fe:runtime_tests.exe > "$WORK/rt.log" 2>&1; then
  pass "runtime_tests compile"
  OUT=$(wine ./runtime_tests.exe 2>&1)
  if grep -q '^PASS' <<< "$OUT"; then pass "runtime_tests pass ($(grep -o '[0-9]* checks' <<< "$OUT" | head -1))"
  else fail "runtime_tests" "$(tail -12 <<< "$OUT")"; fi
else
  fail "runtime_tests compile" "$(grep -E 'error C' "$WORK/rt.log" | head -10)"
fi

if cl /nologo /std:c++17 /EHsc /permissive- /Zc:__cplusplus /utf-8 /O2 /W3 \
     /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS \
     /I"$ROOT/runtime/include" /I"$ROOT/editor/tests/stub" /I"$ROOT/editor/src" /I"$VKSDK/Include" \
     "$ROOT/editor/tests/gizmo_tests.cpp" "$ROOT/editor/src/gizmo.cpp" \
     "$ROOT/runtime/src/pv_math.cpp" /Fe:gizmo_tests.exe > "$WORK/gz.log" 2>&1; then
  pass "gizmo_tests compile"
  OUT=$(wine ./gizmo_tests.exe 2>&1)
  if grep -q '^PASS' <<< "$OUT"; then pass "gizmo_tests pass ($(grep -o '[0-9]* checks' <<< "$OUT" | head -1))"
  else fail "gizmo_tests" "$(tail -12 <<< "$OUT")"; fi
else
  fail "gizmo_tests compile" "$(grep -E 'error C' "$WORK/gz.log" | head -10)"
fi

section "5. Headless launch behaviour of the Windows binary"
GBIN="$ROOT/examples/mini_game/build/bin/mini_game.exe"
if [ -f "$GBIN" ]; then
  OUT=$( cd "$(dirname "$GBIN")" && timeout 40 wine ./mini_game.exe 2>&1 )
  RC=$?
  if [ $RC -eq 0 ] && grep -qi 'display\|window' <<< "$OUT"; then
    pass "exits cleanly with a diagnostic when no display is present"
  else
    fail "headless launch (exit $RC)" "$(tail -6 <<< "$OUT")"
  fi
else
  fail "mini_game.exe was not produced"
fi

section "6. Live interop with a PlainS server, both built by MSVC"
PLS_ROOT=${PLS_ROOT:-}
if [ -z "$PLS_ROOT" ] && [ -d "$ROOT/../PlainS" ]; then PLS_ROOT="$(cd "$ROOT/../PlainS" && pwd)"; fi
if [ -z "$PLS_ROOT" ]; then
  printf '  %sSKIP%s  interop (PlainS not found next door; set PLS_ROOT to enable)\n' $'\033[33m' "$OFF"
else
  mkdir -p "$WORK/interop" && cd "$WORK/interop"
  if cl /nologo /std:c++17 /EHsc /permissive- /Zc:__cplusplus /utf-8 /O2 /W3 \
       /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS \
       /I"$ROOT/runtime/include" /I"$VKSDK/Include" \
       "$ROOT/runtime/tests/network_interop_harness.cpp" "$ROOT/runtime/src/network.cpp" \
       "$ROOT/runtime/src/network_scene.cpp" "$ROOT/runtime/src/pv_value.cpp" \
       "$ROOT/runtime/src/pv_json.cpp" /Fe:interop.exe /link ws2_32.lib > "$WORK/io.log" 2>&1; then
    pass "interop harness compiles with MSVC"
    SRVBIN="$PLS_ROOT/examples/mp_server/build/bin/mp_server.exe"
    if [ ! -f "$SRVBIN" ]; then
      fail "PlainS mp_server.exe not built -- run PlainS/scripts/verify_msvc.sh first"
    else
      ( cd "$(dirname "$SRVBIN")" && setsid nohup wine ./mp_server.exe > "$WORK/srv.log" 2>&1 < /dev/null & echo $! > "$WORK/srv.pid" )
      sleep 8
      OUT=$(cd "$WORK/interop" && wine ./interop.exe 2>&1)
      if grep -qE '([0-9]+)/\1 checks passed' <<< "$OUT"; then
        pass "interop: $(grep -o '[0-9]*/[0-9]* checks passed' <<< "$OUT")"
      else
        fail "interop" "$(tail -14 <<< "$OUT")"
      fi
      kill -9 "$(cat "$WORK/srv.pid")" 2>/dev/null
      wineserver -k 2>/dev/null; sleep 2
    fi
  else
    fail "interop harness compile" "$(grep -E 'error C' "$WORK/io.log" | head -10)"
  fi
fi

section "Summary"
printf '  %d passed, %d failed\n' "$PASSED" "$FAILED"
if [ "$FAILED" -gt 0 ]; then
  printf '  failed checks:\n'; for n in "${FAILED_NAMES[@]}"; do printf '    - %s\n' "$n"; done
  printf '\n  %sMSVC VERIFICATION FAILED%s\n' "$RED" "$OFF"; exit 1
fi
printf '\n  %sALL MSVC CHECKS PASSED%s\n' "$GREEN" "$OFF"
