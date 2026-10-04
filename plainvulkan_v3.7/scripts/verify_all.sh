#!/usr/bin/env bash
# verify_all.sh -- end-to-end verification of the whole PlainVulkan toolchain.
#
# Runs every check that does not require a GPU or a display:
#   1. Rust workspace builds clean and its test suite passes
#   2. The C++ runtime compiles, and both C++ test suites pass
#   3. Every Pv:: command has one implementation and matching arity tables
#   4. Every example (plus a generated `pv new` project) parses, generates
#      C++, compiles, and links to a native binary
#   5. A synthesized program referencing all 268 commands links, proving no
#      command is declared without a definition
#   6. Each built binary runs and exits cleanly when no display is present
#
# Usage:  bash scripts/verify_all.sh [--release] [--quick]
#   --release  build with optimizations (Release instead of RelWithDebInfo)
#   --quick    skip the per-example full CMake builds; still compiles and
#              links every generated main.cpp against a single shared runtime
#
# Exit status is 0 only if every check passed.
set -uo pipefail

PV_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PV_ROOT"

RELEASE=0
QUICK=0
for arg in "$@"; do
  case "$arg" in
    --release) RELEASE=1 ;;
    --quick)   QUICK=1 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

WORK="${PV_VERIFY_WORKDIR:-/tmp/pv_verify}"
mkdir -p "$WORK"
JOBS="$(nproc 2>/dev/null || echo 2)"

PASS=0
FAIL=0
FAILED_NAMES=()

say()  { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }
ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m  %s\n' "$*"; }
bad()  { FAIL=$((FAIL+1)); FAILED_NAMES+=("$1"); printf '  \033[31mFAIL\033[0m  %s\n' "$1"; }

# check <name> <logfile> <command...>
check() {
  local name="$1"; shift
  local log="$1"; shift
  if "$@" > "$log" 2>&1; then ok "$name"; else bad "$name"; echo "        (log: $log)"; tail -15 "$log" | sed 's/^/        | /'; fi
}

# ---------------------------------------------------------------- Rust ---
say "1. Rust transpiler + compiler"
CARGO_FLAGS=(--release)
check "cargo build"  "$WORK/cargo_build.log" cargo build "${CARGO_FLAGS[@]}"
check "cargo test"   "$WORK/cargo_test.log"  cargo test  "${CARGO_FLAGS[@]}"

PVC="$PV_ROOT/target/release/pvc"
PVCC="$PV_ROOT/target/release/pvcc"
[ -x "$PVC" ]  && ok "pvc binary exists"  || bad "pvc binary exists"
[ -x "$PVCC" ] && ok "pvcc binary exists" || bad "pvcc binary exists"

# ------------------------------------------------------- C++ unit tests ---
say "2. C++ test suites"
check "runtime_tests compile" "$WORK/rt_build.log" \
  c++ -std=c++17 -Iruntime/include \
      -Iruntime/third_party/Ai/RVO2/src \
      -Iruntime/third_party/Ai/recastnavigation/Recast/Include \
      -Iruntime/third_party/Ai/recastnavigation/Detour/Include \
      -DRVO_STATIC_DEFINE -o "$WORK/runtime_tests" \
      runtime/tests/runtime_tests.cpp runtime/src/pv_math.cpp \
      runtime/src/pv_json.cpp runtime/src/anim_sample.cpp runtime/src/pv_value.cpp \
      runtime/src/terrain.cpp runtime/src/ai.cpp runtime/src/ai_navmesh.cpp \
      runtime/third_party/Ai/recastnavigation/Recast/Source/*.cpp \
      runtime/third_party/Ai/recastnavigation/Detour/Source/*.cpp \
      runtime/third_party/Ai/RVO2/src/*.cc
check "runtime_tests pass"    "$WORK/rt_run.log"   "$WORK/runtime_tests"

check "gizmo_tests compile"   "$WORK/gz_build.log" \
  c++ -std=c++17 -Iruntime/include -Ieditor/tests/stub -Ieditor/src \
      -o "$WORK/gizmo_tests" editor/tests/gizmo_tests.cpp editor/src/gizmo.cpp \
      runtime/src/pv_math.cpp
check "gizmo_tests pass"      "$WORK/gz_run.log"   "$WORK/gizmo_tests"

check "command coverage"      "$WORK/cov.log"      bash scripts/check_command_coverage.sh

# ------------------------------------------------------------- parsing ---
say "3. Parsing every .pv source"
for f in examples/*/main.pv; do
  check "parse $(basename "$(dirname "$f")")" "$WORK/parse_$(basename "$(dirname "$f")").log" "$PVC" check "$f"
done

# ------------------------------------------- all-commands link coverage ---
say "4. Link coverage for all Pv:: commands"
ALLCMD="$WORK/allcmds"
mkdir -p "$ALLCMD"
python3 - "$ALLCMD" <<'PYEOF'
import re, sys, json, os
root = os.path.dirname(os.path.dirname(os.path.abspath(sys.argv[0]))) if False else os.getcwd()
src = open(os.path.join(root, 'runtime/include/pv/pv_runtime.h')).read()
src = re.sub(r'//[^\n]*', '', src)
decls = re.findall(r'\bValue\s+([A-Za-z_]\w*)\s*\(([^;]*?)\)\s*;', src, re.S)
# The four network callbacks take a function pointer, not a Value, so they
# are referenced through a real handler function rather than dummy args.
CB = {'OnConnect': 0, 'OnDisconnect': 1, 'OnReceive': 1, 'OnNetworkError': 1}
lines = ['function CbA() { Pv::Log(1); }', 'function CbB(x) { Pv::Log(x); }',
         'Pv::Init();', 'var guard = Pv::GetFPS();', 'if (guard < -1.0) {']
n = 0
for name, params in decls:
    n += 1
    if name == 'Init':
        continue
    if name in CB:
        lines.append('    Pv::%s(%s);' % (name, 'CbA' if CB[name] == 0 else 'CbB'))
        continue
    p = params.strip()
    arity = 0 if not p else len([x for x in p.split(',') if x.strip()])
    lines.append('    Pv::%s(%s);' % (name, ', '.join(['1'] * arity)))
lines += ['}', 'Pv::Shutdown();']
out = sys.argv[1]
open(os.path.join(out, 'main.pv'), 'w').write('\n'.join(lines) + '\n')
json.dump({"name": "allcmds", "version": "0.1.0", "entry": "main.pv",
           "window": {"width": 320, "height": 240, "title": "All Commands"},
           "build": {}}, open(os.path.join(out, 'pvproject.json'), 'w'), indent=2)
print("referenced %d commands" % n)
with open(os.path.join(out, '.command_count'), 'w') as f:
    f.write(str(n))
PYEOF
# Read the count back rather than hardcoding it: the label used to say
# "all 268 commands link" and stayed at 268 after the terrain and AI
# commands landed, which made a growing surface look like a static one.
CMD_COUNT="$(cat "$ALLCMD/.command_count" 2>/dev/null || echo '?')"
BUILD_ARGS=(build "$ALLCMD" --runtime-dir "$PV_ROOT/runtime")
[ "$RELEASE" = 1 ] && BUILD_ARGS+=(--release)
check "all $CMD_COUNT commands link" "$WORK/allcmds.log" "$PVCC" "${BUILD_ARGS[@]}"

# The runtime archive built above is reused below so every example doesn't
# recompile the identical 30-file runtime.
SHARED_LIB="$(find "$ALLCMD/build/_cmake" -name libpv_runtime.a 2>/dev/null | head -1)"

# ----------------------------------------------------------- examples ----
say "5. Example projects"
EXAMPLES=(hello_triangle mini_game physics_playground multiplayer_client syntax_showcase terrain_ai)

# A `pv new` project, to cover the starter template users actually get.
rm -rf "$WORK/StarterGame"
(cd "$WORK" && python3 "$PV_ROOT/pv.py" new StarterGame > /dev/null 2>&1)

if [ "$QUICK" = 1 ] && [ -n "$SHARED_LIB" ]; then
  for p in "${EXAMPLES[@]}"; do
    d="examples/$p"
    "$PVCC" build "$PV_ROOT/$d" --runtime-dir "$PV_ROOT/runtime" > "$WORK/gen_$p.log" 2>&1 &
  done
  wait
  for p in "${EXAMPLES[@]}"; do
    gen="$PV_ROOT/examples/$p/build/generated/main.cpp"
    if [ -f "$gen" ]; then
      check "link $p" "$WORK/link_$p.log" \
        c++ -std=c++17 -Iruntime/include -o "$WORK/bin_$p" "$gen" "$SHARED_LIB" \
            -lvulkan -lglfw -lpthread -ldl -lm
    else
      bad "link $p (no generated main.cpp)"
    fi
  done
else
  for p in "${EXAMPLES[@]}"; do
    A=(build "$PV_ROOT/examples/$p" --runtime-dir "$PV_ROOT/runtime")
    [ "$RELEASE" = 1 ] && A+=(--release)
    check "build $p" "$WORK/build_$p.log" "$PVCC" "${A[@]}"
  done
  A=(build "$WORK/StarterGame" --runtime-dir "$PV_ROOT/runtime")
  [ "$RELEASE" = 1 ] && A+=(--release)
  check "build StarterGame (pv new template)" "$WORK/build_starter.log" "$PVCC" "${A[@]}"
fi

# ------------------------------------------------------------- running ---
say "6. Running built binaries (headless: must fail gracefully, not crash)"
run_binary() {
  local name="$1" bin="$2"
  [ -x "$bin" ] || { bad "run $name (missing binary)"; return; }
  local out; out="$(timeout 30 "$bin" 2>&1)"; local rc=$?
  # 0 = clean exit. 124 = timeout. Anything >= 128 is a signal (segfault,
  # abort) -- that is the failure mode this check exists to catch.
  if [ "$rc" -ge 128 ]; then
    bad "run $name (died with signal, rc=$rc)"; echo "$out" | tail -5 | sed 's/^/        | /'
  elif [ "$rc" = 124 ]; then
    bad "run $name (hung)"
  else
    ok "run $name (exit $rc, handled no-display cleanly)"
  fi
}
for p in "${EXAMPLES[@]}"; do
  b="$PV_ROOT/examples/$p/build/bin/$p"
  [ -x "$b" ] || b="$WORK/bin_$p"
  run_binary "$p" "$b"
done

# ---------------------------------------------------------- regressions ---
say "7. Regression checks for previously fixed bugs"
mkproj() {
  local d="$WORK/rg_$1"; rm -rf "$d"; mkdir -p "$d"
  printf '%s\n' "$2" > "$d/main.pv"
  printf '{"name":"rg%s","version":"0.1.0","entry":"main.pv","window":{"width":320,"height":240,"title":"T"},"build":{}}\n' "$1" > "$d/pvproject.json"
  echo "$d"
}
# codegen must SUCCEED and produce compilable C++
expect_compiles() {
  local name="$1" d="$2"
  "$PVCC" build "$d" --runtime-dir "$PV_ROOT/runtime" > "$WORK/rg_$name.log" 2>&1
  local gen="$d/build/generated/main.cpp"
  if [ ! -f "$gen" ]; then bad "regression $name (codegen refused)"; return; fi
  if c++ -std=c++17 -Iruntime/include -c "$gen" -o /dev/null > "$WORK/rg_${name}_cxx.log" 2>&1; then
    ok "regression $name (generated C++ compiles)"
  else
    bad "regression $name (generated C++ does not compile)"
    head -6 "$WORK/rg_${name}_cxx.log" | sed 's/^/        | /'
  fi
}
# codegen must FAIL with a message containing a given needle
expect_diagnostic() {
  local name="$1" d="$2" needle="$3"
  if "$PVCC" build "$d" --runtime-dir "$PV_ROOT/runtime" > "$WORK/rg_$name.log" 2>&1; then
    bad "regression $name (expected a diagnostic, build succeeded)"
  elif grep -q "$needle" "$WORK/rg_$name.log"; then
    ok "regression $name (clean diagnostic)"
  else
    bad "regression $name (wrong diagnostic)"; head -5 "$WORK/rg_$name.log" | sed 's/^/        | /'
  fi
}

expect_compiles "toplevel_return"  "$(mkproj toplevel_return  'Pv::Init();
return 3;')"
expect_compiles "toplevel_return_bare" "$(mkproj toplevel_return_bare 'Pv::Init();
if (true) { return; }')"
expect_compiles "callback_ok" "$(mkproj callback_ok 'function OnMsg(d) { Pv::Log(d); }
Pv::Init();
Pv::OnReceive(OnMsg);')"
expect_diagnostic "callback_literal" "$(mkproj callback_literal 'Pv::OnConnect(1);')" \
  "must be the name of a top-level"
expect_diagnostic "function_as_value" "$(mkproj function_as_value 'function H() { Pv::Log(1); }
Pv::Log(H);')" "can.t hold a function"
# a rendered diagnostic must not be prefixed with a second "error:"
RG_PARSE="$(mkproj parse_error 'Pv::Init(')"
if "$PVCC" build "$RG_PARSE" --runtime-dir "$PV_ROOT/runtime" 2>&1 | grep -q "error: error:"; then
  bad "regression doubled_error_prefix"
else
  ok "regression doubled_error_prefix (single 'error:' prefix)"
fi

# ------------------------------------------------------ network interop ---
# Builds the real client networking code and drives it against a
# spec-conformant mock PlainS server, so the wire format, framing, callback
# dispatch, state deserialization and latency probe are all executed rather
# than assumed. Skipped only if port 8080 can't be bound.
say "8. Network interop (real client vs. mock PlainS server)"
check "interop harness compiles" "$WORK/interop_build.log" \
  c++ -std=c++17 -Iruntime/include -o "$WORK/interop" \
      runtime/tests/network_interop_harness.cpp runtime/src/network.cpp \
      runtime/src/network_scene.cpp runtime/src/pv_value.cpp runtime/src/pv_json.cpp -lpthread

if [ -x "$WORK/interop" ]; then
  python3 runtime/tests/mock_plains_server.py --port 8080 --quiet > "$WORK/mock_server.log" 2>&1 &
  MOCK_PID=$!
  sleep 2
  if timeout 90 "$WORK/interop" > "$WORK/interop_run.log" 2>&1; then
    ok "interop: $(grep -oE '[0-9]+/[0-9]+ checks passed' "$WORK/interop_run.log" | tail -1)"
  else
    bad "network interop against mock server"
    tail -12 "$WORK/interop_run.log" | sed 's/^/        | /'
  fi
  kill "$MOCK_PID" 2>/dev/null
  wait "$MOCK_PID" 2>/dev/null
fi

# ---------------------------------------------------------------- done ---
say "Summary"
printf '  %d passed, %d failed\n' "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
  printf '  failed checks:\n'
  for n in "${FAILED_NAMES[@]}"; do printf '    - %s\n' "$n"; done
  exit 1
fi
printf '\n  \033[32mALL CHECKS PASSED\033[0m\n'
exit 0
