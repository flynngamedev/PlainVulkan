# PlainVulkan 3.7

A `.pv` language that compiles to native C++ and links against a Vulkan
runtime. `pv.py` is the CLI: parse → generate C++ → CMake build → run.

Pair it with **PlainServer 2.6** (`plainserver_v3`, `pls.py`) for
authoritative multiplayer.

```
main.pv  →  transpiler (lexer + LALRPOP)  →  AST
         →  compiler (codegen)            →  generated C++
         →  CMake + runtime/              →  native Vulkan binary
```

## Requirements

- Python 3 (to run `pv.py`)
- Rust (`cargo`) — first `pv build` compiles the transpiler and compiler
- CMake 3.16+ and a C++17 compiler
- Vulkan SDK (headers, loader, `glslangValidator` or `glslc`)

```bash
python pv.py doctor    # reports what is missing
python pv.py version   # PlainVulkan 3.7
```

**Windows:** Visual Studio with the C++ workload. Run from an **x64 Native
Tools Command Prompt** so `cl` is on PATH. GLFW is fetched if needed.

If `cargo` is the **GNU** Windows toolchain (`x86_64-pc-windows-gnu`),
`windows-sys` needs MinGW `dlltool.exe` and the build fails with
`error calling dlltool 'dlltool.exe': program not found`. `pv.py` then
uses `cargo +stable-x86_64-pc-windows-msvc`. To make MSVC the default:

```
rustup toolchain install stable-x86_64-pc-windows-msvc
rustup default stable-x86_64-pc-windows-msvc
```

```
winget install Rustlang.Rustup Kitware.CMake LunarG.VulkanSDK
```

**Linux (Ubuntu/Debian):**

```bash
apt install cargo cmake g++ libvulkan-dev vulkan-validationlayers \
            glslang-tools libglfw3-dev
```

**macOS:** Vulkan SDK (MoltenVK) plus Xcode command line tools.

Copy `pv.py` onto PATH as `pv` if you want a short command. On Windows,
`python pv.py ...` from this repo is enough.

## Quick start

```bash
python pv.py new MyGame
cd MyGame
python pv.py run .
```

`pv new` creates `assets/`, `scripts/`, `main.pv`, and `pvproject.json`.

Examples in this tree:

```bash
python pv.py run examples/hello_triangle
python pv.py check examples/syntax_showcase/main.pv
python pv.py run examples/mini_game
python pv.py run examples/terrain_ai
```

Keyboard input uses GLFW integer key codes (`87` is W, `65` is A, `32` is
Space, `256` is Escape).

## CLI

```
pv new <name>           Create a project
pv build <dir>          Parse, generate C++, compile
pv run <dir>            Build if needed, then run
pv check <file.pv>      Parse and report syntax errors
pv edit [scene.pvscene] Scene editor
pv doctor               Check cargo, cmake, C++, Vulkan/GLSL
pv version
```

Games are GUI-subsystem binaries on Windows. `pv run` from a terminal
attaches logs to that terminal. Set `PV_CONSOLE=1` to force a console when
you start the `.exe` yourself.

## Editor

```bash
python pv.py edit                 # empty scene (floor + light)
python pv.py edit myscene.pvscene
```

The viewport is the engine renderer. Play uses the same path a shipped
game does. Scenes are `.pvscene` JSON (intent, not runtime handles).

| Input | Action |
|---|---|
| Click | Select |
| Hold RMB | Fly (WASD, Q/E, Shift sprint) |
| W / E / R | Move / Rotate / Scale gizmo |
| X | World / local gizmo |
| F | Frame selection |
| Delete | Delete selection |
| Ctrl+S | Save |

## Physics

NVIDIA PhysX when found (`PHYSX_ROOT`, `-DPHYSX_ROOT=`, or a system
package). Otherwise the builtin solver, with a warning. Opt-in source
build: `-DPV_FETCH_PHYSX=ON`.

`Pv::GetPhysicsBackend()` reports which one is live.
`PV_PHYSICS_BACKEND=builtin` forces the fallback.

## Navigation (Recast, Detour, RVO2)

`Pv::CreateNavGrid`, `Pv::FindPath`, `Pv::CreateAgent`, `Pv::UpdateAgents`
and related commands are unchanged. Internally:

- **Recast** builds a navmesh from walkable grid cells
- **Detour** queries it
- **RVO2** does local avoidance for agents

Vendored, trimmed copies live in `runtime/third_party/Ai/` (see that
folder’s README). Pathfinding still refuses sealed walls and diagonal
corner-cuts.

## Multiplayer

PlainVulkan is the client. Start **PlainServer 2.6** first, then the
client. Shared TCP frames: length-prefixed, JSON payloads.

```bash
# terminal 1 — PlainServer tree
python pls.py run examples/mp_server          # 127.0.0.1:8080

# terminal 2 — this tree
python pv.py run examples/multiplayer_client  # Pv::Connect("127.0.0.1", 8080)
```

The client sends input (`Pv::SerializeInput`); the server owns positions
and broadcasts `state`. See `PLAINVULKAN_REFERENCE.md` (Networking) and
PlainServer’s README.

## Tests (no GPU)

```bash
# from runtime/, with Recast/Detour/RVO2 on the include/source path
# (exact flags are in the header of runtime/tests/runtime_tests.cpp)
bash scripts/check_command_coverage.sh
cd transpiler && cargo test
```

On Windows, `scripts/verify_msvc.sh` documents an MSVC build; from a
Developer Prompt you can compile `runtime_tests` the same way as the
comment in `runtime_tests.cpp`.

## Layout

```
pv.py                      CLI
transpiler/                lexer, LALRPOP grammar, AST → pvc
compiler/                  C++ codegen, Pv:: arity table (~325 commands) → pvcc
runtime/                   Vulkan engine (static lib)
  include/pv/              Value + Pv:: API
  src/                     implementations
  third_party/             stb, miniaudio, cgltf, Monocypher, Ai/…
editor/                    pvedit (ImGui, linked to pv_runtime)
examples/
PLAINVULKAN_REFERENCE.md   language + engine reference
scripts/                   coverage and verify helpers
```

`runtime/README.md` lists what is fully implemented vs simplified vs
stubbed.

## License notes

Third-party licenses stay next to the vendored sources. Recast/Detour are
zlib-style (`runtime/third_party/Ai/recastnavigation/License.txt`). RVO2
is Apache-2.0 (`runtime/third_party/Ai/RVO2/LICENSE`).
