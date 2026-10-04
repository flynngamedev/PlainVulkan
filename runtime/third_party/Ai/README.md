# Vendored AI libraries

PlainVulkan 3.7 compiles only these trees into `pv_runtime`:

| Library | Upstream | Used for |
|---|---|---|
| Recast + Detour | https://github.com/recastnavigation/recastnavigation | Navmesh build + query |
| RVO2 | https://github.com/snape/RVO2 | Agent local avoidance |

Demo apps, tests, docs, CI, extra Recast modules (DetourCrowd,
DetourTileCache, DebugUtils), and RVO2 examples/build files are **not**
shipped. Licenses are kept next to the remaining sources.

CMake globs:

- `recastnavigation/Recast/Source/*.cpp`
- `recastnavigation/Detour/Source/*.cpp`
- `RVO2/src/*.cc`

Include dirs: `Recast/Include`, `Detour/Include`, `RVO2/src`.
`RVO_STATIC_DEFINE` is set for a static RVO2 build.
