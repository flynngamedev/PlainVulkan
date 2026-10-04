# PlainVulkan runtime -- what's real, what's simplified, what's stubbed

The runtime backs all 323 `Pv::` commands with real C++ symbols (cross-checked
against `compiler/src/pv_commands.rs` by `../scripts/check_command_coverage.sh`
-- every declared function has exactly one definition, nothing is missing).
"Backed by a real symbol" is not the same as "fully implemented," though, so
this file is the honest map of which is which. Nothing here silently pretends
to work: anything below Tier 2 logs a clear `[PlainVulkan][WARNING]` the first
time it's meaningfully used, telling you it's simplified and pointing back
here.

Tiers:
- **Tier 1** -- genuinely complete for a small engine; no known gaps.
- **Tier 2** -- fully functional, straightforward mappings onto a real
  library (GLFW, miniaudio, stb_image, ...).
- **Tier 3** -- really works, with a deliberate scope simplification
  documented inline (e.g. capsule colliders shade like spheres).
- **Tier 4** -- the handle/API is real and safe to call (never crashes),
  but the command doesn't yet have its real effect. Logs a warning.

## Core / window / frame (Tier 1-2)
GLFW + a real Vulkan instance/device/swapchain/render pass, 2 frames in
flight, resize handling. `Pv::SetVSync` triggers a swapchain rebuild.
`Pv::SetFullscreen` uses the primary monitor. Delta time and FPS are real
(`std::chrono`).

## 2D drawing (Tier 2-3)
Triangle/rect/circle/line/sprite all draw for real via a small pipeline with
alpha blending; draw order is preserved exactly (each call issues its draw
immediately into the frame's command buffer -- see `draw2DVerts` in
`vk_core.cpp` -- rather than being batched and reordered). `DrawTriangle`
only takes one anchor point in the reference, so it draws a small fixed-size
triangle there. Text (`DrawText`/`DrawTextEx`) uses a compact built-in 3x5
bitmap font (`font_data.cpp`) covering A-Z/0-9/basic punctuation, not a real
typeface -- swapping in `stb_truetype` + a `.ttf` is the natural upgrade
path and doesn't require touching anything else.

## 3D drawing (Tier 2-3)
Real depth-tested forward rendering with up to 4 lights (ambient + point/
directional; spot lights shade as point lights, no cone falloff).
`DrawBillboard` is genuinely camera-facing (built from the view matrix's own
right/up vectors, not Euler angles). `SetWireframe` uses a real second
pipeline variant (falls back to solid if the GPU lacks `fillModeNonSolid`).
`DrawSkybox` is a Tier 4 stub -- no cubemap sampling yet. `DrawTerrain` used
to be one too; it is a real implementation now (see the Terrain section
below).

## Meshes (Tier 2)
`.obj` via tinyobjloader, `.gltf`/`.glb` via cgltf -- both real parses of
POSITION/NORMAL/TEXCOORD_0 into GPU buffers. Simplification: only the first
mesh primitive of a glTF file is imported (no node hierarchy, multi-mesh
scenes, or skinning). `CreateCube/Sphere/Plane/Cylinder` are genuine
procedural geometry. `UpdateMesh` is a Tier 4 stub (recreate the mesh
instead).

## Textures (Tier 2-4)
PNG/JPG via stb_image, real GPU upload, real sampling. `LoadCubemap` loads
the file as a flat 2D texture rather than 6 cube faces (Tier 4, matching
`DrawSkybox`'s gap). `CreateRenderTexture` allocates a real sampleable
texture but nothing renders into it yet (Tier 4). `SetTextureFilter`/
`SetTextureWrap` are tracked-but-inert (Tier 4; all textures use linear
filtering + repeat wrap).

## Shaders (Tier 4)
Handles are tracked and every call is safe, but custom shaders don't replace
the built-in pipelines -- see the comment at the top of `shader.cpp` for
what a real implementation would need (SPIR-V compilation + a pipeline
cache keyed by shader+vertex-format).

## Lighting / materials (Tier 2-4)
Ambient + up to 4 point/directional lights genuinely affect shading every
frame. Materials store real values but -- because the command reference has
no "assign this material to that mesh" call -- `DrawMesh` doesn't yet bind a
material's albedo (Tier 4; always uses the default white texture). Shadows
and fog are tracked flags without a rendering effect yet (Tier 4).

## Audio (Tier 2)
Real playback via miniaudio. WAV/MP3/FLAC decode with miniaudio's built-in
decoders. OGG decodes through a real custom `ma_decoding_backend_vtable`
wrapping the vendored `stb_vorbis.c` (miniaudio's own documented extension
point, not a hack) -- see `audio.cpp`'s header comment.

## Input (Tier 1-2)
Keyboard/mouse fully real via GLFW polling with correct per-frame edge
detection for Pressed/Released (stable no matter how many times you query
the same key in one frame -- see `input.cpp`). Gamepad buttons/axes are
real; `SetGamepadVibration` is a no-op (Tier 4 -- GLFW has no rumble API).

## Physics (Tier 1-2 with PhysX; Tier 3 on the fallback)
Physics now runs on **NVIDIA PhysX** through a backend interface
(`include/pv/pv_physics.h`), with the original hand-written solver kept as a
fallback. Which one you get:

| Build | Backend | File |
|---|---|---|
| `-DPV_WITH_PHYSX=ON` (default) and the SDK is found | PhysX | `physics_physx.cpp` |
| SDK not found, or `-DPV_WITH_PHYSX=OFF` | builtin | `physics_builtin.cpp` |

`Pv::GetPhysicsBackend()` reports which is live at runtime, and setting
`PV_PHYSICS_BACKEND=builtin` forces the fallback without a rebuild, which is
how you bisect a suspected PhysX bug against the old behaviour.

**What PhysX adds over the solver it replaced:** a real constraint solver
(stacks and resting contact are stable instead of jittering apart), genuine
rotational dynamics (torque, angular velocity, inertia tensors derived from
the shape -- the old solver had *no rotation at all*), real capsule shapes
rather than capsules-treated-as-spheres, continuous collision detection so a
fast projectile stops tunnelling through thin walls, friction and
restitution via `PxMaterial`, a real broadphase so body count stops being
O(n^2), contact and trigger events, and per-axis position/rotation locks.

Both backends step at a **fixed internal timestep** with accumulation
(`Pv::SetPhysicsTimestep`), because a variable timestep makes any constraint
solver behave differently frame to frame -- a stack stable at 144fps
explodes at 30fps.

Set `PV_PHYSX_PVD=<host>` to attach the PhysX Visual Debugger. It's opt-in
because it opens a socket.

The **builtin fallback** is the original system, unchanged in behaviour:
gravity, semi-implicit Euler, AABB/sphere resolution, ray-sphere and
ray-AABB casts. Its limits are now queryable rather than only documented
(`supportsRotation()` returns false), and the scene system uses that to
avoid overwriting an entity's authored rotation with a meaningless one.
It logs a clear warning at `Pv::InitPhysics` so it can't be mistaken for
the real backend.

## Scene system (Tier 2) -- new
A hierarchical entity/component scene graph (`include/pv/pv_scene.h`,
`scene.cpp`, `scene_io.cpp`, `scene_commands.cpp`) with parenting, tags,
and eight component types (mesh, light, rigid body, particle emitter,
animator, camera, sprite, text). Scenes serialize to `.pvscene`, a
pretty-printed JSON format that diffs and merges in git.

The format stores *intent*, never runtime handles: an entity records that
it's a 2m cube of mass 5, and `sceneStart` re-creates the mesh and the
PhysX actor from that. Every field is optional on load and defaults to the
component struct's value, so a scene written by an older build still loads.
Saves are atomic (write to a temp file, then rename) so an interrupted save
can't destroy your work.

Deliberate design call: this is a plain entity/component tree, **not** an
archetype ECS. An ECS's win is cache-coherent iteration over huge
homogeneous entity counts, and it costs a lot of indirection to get there. A
PlainVulkan scene is hundreds of hand-authored entities, per-frame cost is
dominated by draw submission, and what actually matters is that an editor
can point at one entity and show you its fields.

## Particles (Tier 2) -- new
CPU-simulated, GPU-batched emitters (`pv_particles.h`, `particles.cpp`,
`shaders/particle.{vert,frag}`). Six emitter shapes, min/max variance on
every initial property, four-point lifetime curves for size and alpha,
gravity/drag/wind/vortex forces, sprite-sheet animation, and
world-vs-local simulation space.

Rendering builds camera-facing quads on the CPU into a per-frame ring
buffer and issues one draw per emitter. Alpha-blended emitters are sorted
back-to-front (out-of-order alpha composites visibly wrong); additive ones
skip the sort entirely, since addition is commutative -- a real reason to
prefer additive for large spark and fire effects. Both variants depth-*test*
but never depth-*write*, which is what stops the far half of a smoke plume
from disappearing.

Simulation is on the CPU deliberately: a compute-shader system is faster in
the large, but it needs a compute queue, storage buffers, indirect draw, and
a readback story. For the thousands-not-millions counts a PlainVulkan game
uses, a tight CPU loop is simpler, portable to every device the runtime
already supports, and leaves particle state directly inspectable by the
editor -- which matters a lot for authoring.

## Networking (Tier 1-2) -- new

`runtime/src/network.cpp` (transport) and `network_scene.cpp` (the
serialization helpers that touch the scene). Neither includes
`pv_internal.h`, so neither pulls in Vulkan or GLFW -- the whole networking
half compiles and can be exercised with no graphics stack present, which is
also what let it be tested against a live PlainS server.

Real TCP with length-prefixed framing (`pv_net.h`), one background reader
thread, `TCP_NODELAY` on every socket, non-blocking connect with a 5-second
timeout, and a once-a-second PING/PONG probe for latency. Winsock on
Windows, BSD sockets elsewhere. Peer-to-peer (`Pv::Listen` /
`AcceptConnection` / `SendToPeer`) shares the same reader thread and frame
format.

**Verified end to end**, not just compiled: the client code in this
directory was linked against a stub scene layer and run against a PlainS
server built from `PlainS/examples/mp_server` -- connect, welcome packet,
input round trip (server-computed position mirrored back into a local
entity), latency probe, and clean disconnect. 13/13 checks.

Deliberate scope notes, all documented inline at the point they matter:

- **`Pv::Flush` is a no-op** (Tier 1 by intent). Sends are unbuffered and
  `TCP_NODELAY` is set, so there is nothing to force. Kept so scripts
  written against the reference's buffered-send model still run.
- **`Pv::GetPacketLoss` reports unanswered latency probes**, not datagram
  loss. TCP never exposes a loss rate, so reporting one would be fiction.
- **Callbacks are function pointers**, so they must be top-level `.pv`
  functions. A nested function compiles to a capturing lambda and will be
  rejected by the C++ compiler rather than silently misbehaving.
- **`Pv::OnReceive` takes ownership of the message stream** -- with a
  handler registered, nothing reaches `Pv::Receive`/`PollMessages`. This
  prevents a script that mixes both styles from processing every message
  twice.
- **Remote entities are mirrored as `Net_<serverId>`** and auto-created on
  first sight by `Pv::DeserializeState`. An entity the server marks `"dead"`
  is deactivated, not destroyed, so a respawn under the same id works and
  script-held handles stay valid.

## Platform support (Tier 1)
Windows, Linux, and macOS. `pv_platform.h` is the single place per-OS and
per-compiler differences live -- it sets `NOMINMAX` and `NOGDI` before
anything can pull in `windows.h` (whose `min`/`max` macros would break every
`std::min` in the runtime, and whose `CreateWindow` macro would rename
`Pv::CreateWindow` out from under us), and provides `PV_PI` because `M_PI`
is POSIX rather than ISO C++.

`exeDirectory()` has real implementations on all three platforms
(`GetModuleFileNameW`, `_NSGetExecutablePath`, `/proc/self/exe`); it
previously returned `"."` on anything but Linux, which broke shader loading
for a double-clicked `.exe`. Games build as GUI-subsystem binaries on
Windows so no console flashes on launch; set `PV_CONSOLE=1` to reattach
`Pv::Log` output to the terminal.

One portability fix worth noting: the runtime now uses `GLFW_INCLUDE_NONE`
and includes `vulkan.h` itself. GLFW pulls in `<GL/gl.h>` by default, which
made this Vulkan-only renderer fail to build on any machine without the
OpenGL development headers installed.

## Filesystem / debug (Tier 1)
Plain `<filesystem>`/`<fstream>`; nothing simplified. Debug 3D shapes
project world points to screen space on the CPU and draw with the 2D line
renderer (no near-plane clipping, so an edge behind the camera is skipped
rather than clipped).

## Animation (Tier 2)
`Pv::LoadAnimation` now performs a **real keyframe import** from
`.gltf`/`.glb` (`animation_gltf.cpp`), replacing the placeholder clip it
used to fabricate. It parses every animation in the file, every
translation/rotation/scale channel, the full node hierarchy (depth-first, so
parents always precede children), all three glTF interpolation modes
(LINEAR, STEP, CUBICSPLINE with proper tangent scaling), and both the float
and normalized-integer quaternion layouts.

`Pv::BlendAnimation` is no longer a tracked no-op: animation runs on up to
four weighted layers that sample independently and combine, with the blend
normalized so the result is independent of layer order.
`Pv::CrossFadeAnimation` fades between clips so a walk-to-run transition is
continuous instead of a visible pop.

Clips can also be built entirely from script with no asset file:
`Pv::CreateAnimation` / `AddAnimationClip` / `AddAnimationKey`, and
`Pv::GetAnimationTransform` exposes the sampled root so an animation curve
can drive anything -- a camera, a light, a UI element -- not only meshes.

**Scope boundary, stated plainly:** this animates *transforms*. Skinned
vertex deformation (a joint-matrix palette skinned in the vertex shader)
needs the mesh importer to carry `JOINTS_0`/`WEIGHTS_0` and is not done. A
clip whose channels target a single node -- which is what essentially every
prop, door, platform, camera move, and emitter animation is -- animates
fully and correctly end to end.

## UI (Tier 3)
A real small immediate-mode layer on top of the 2D primitives and mouse
input: buttons, checkboxes, sliders (drag-to-set), a progress bar, and
single-focus text input (click to focus, type, backspace). No theming, no
nested layout containers.

## Terrain (Tier 2)
`terrain.cpp` + `terrain_render.cpp`. Real heightfield storage, bilinear
height sampling, central-difference normals and slope, fBm value-noise
generation (deterministic from a seed), greyscale-image heightmap loading,
per-chunk LOD mesh generation, a bisecting raycast, and collision.

The split between the two files is deliberate and is the reason any of this
is tested: **`terrain.cpp` contains no Vulkan at all**, so `runtime_tests`
links it directly and asserts on real heights, normals, slopes, LOD choices,
chunk index ranges and ray hits with no GPU present. `terrain_render.cpp` is
the thin GPU wrapper (mesh upload, splat-texture upload, the draw).

Chunks carry their shared seam vertex, so chunk boundaries have no holes.
LOD halves the vertex stride per level over 4 levels, and
`buildTerrainChunkGeometry` steps the stride back down when a chunk's cell
count doesn't divide by it -- which is why `Pv::SetTerrainLOD` insists on a
power-of-two `chunkSize` and says so rather than silently disabling LOD.

**Scope boundaries, stated plainly:**
- Splatting is a **CPU bake** (`Pv::BakeTerrainTexture`) into one albedo
  texture, not a multi-layer shader splat. That is what lets it work through
  the existing material path with no shader changes; the cost is that layer
  detail is limited by the bake resolution rather than being per-pixel.
- Terrain collision is resolved in `physicsStepAndSync` as a **position
  correction after the step**, not as a constraint inside a backend. That
  gets it working on both the PhysX and builtin backends for a small amount
  of code (neither backend knows terrain exists), but a body resting on a
  slope is held up rather than sliding down it. `Pv::TerrainRaycast` is there
  for scripts that need the true surface interaction.
- No runtime terrain deformation with automatic mesh refresh:
  `Pv::SetTerrainHeight` edits the data, and you call
  `Pv::BuildTerrainMesh` when you're done editing.

## AI / navigation (Tier 2)
`ai.cpp`, also entirely Vulkan-free and therefore directly tested.

A* over an 8-connected grid with the true `sqrt(2)` diagonal cost (an
admissible heuristic, so the path returned is optimal, not merely
plausible), no-corner-cutting between diagonally touching blockers, per-cell
cost multipliers, obstacle rectangles layered over a base walkability map,
Bresenham line-of-sight, and string-pulling path smoothing. Agents steer
along the path with a turn-rate limit and follow the ground when the grid
carries terrain height.

`Pv::CreateNavGridFromTerrain` derives walkability from slope and makes
steeper ground *more expensive* rather than merely passable or not, so paths
prefer valleys and still find a steep route when it's the only one.

**Why a grid rather than a polygon navmesh:** this engine's world is
heightmap terrain, which is already a grid. Building a navmesh from a grid
to answer the same queries would add region partitioning, contour tracing
and polygon merging for the one visible benefit -- smooth non-staircase
paths -- that string pulling already provides.

**Scope boundaries:** navigation is 2.5D (pathing is planar, height is
sampled onto it), there is no local avoidance between agents (two agents
routed through the same gap will overlap), and the state machine is a
polling model: the engine stores a state name and a time-in-state, and the
script decides what they mean. See the reference for why.

## If you want to extend something here
Every Tier 3/4 item above names the specific file to open. The engine
state all of them share lives in one place: `include/pv/pv_internal.h`'s
`Engine` struct -- except terrain and AI, which deliberately keep their own
registries in `pv_terrain.h` / `pv_ai.h` so they stay Vulkan-free and
testable.
