// pv_terrain.h -- heightmap terrain: storage, sampling, chunked LOD mesh
// generation, layer splatting and collision queries.
//
// Everything declared here is deliberately free of Vulkan. terrain.cpp
// implements all of it plus the sampling/authoring commands with nothing
// but <vector> and pv_math.h, which is what lets runtime_tests link the
// terrain logic directly and assert on real numbers -- heights, normals,
// slopes, LOD selection, raycast hits -- on a machine with no GPU.
//
// The GPU half lives in terrain_render.cpp: mesh upload, texture bake and
// the draw call. That file is the only one that includes pv_internal.h,
// and it is a thin wrapper over the geometry this header produces.
//
// Coordinate convention: X/Z is the ground plane, Y is up, matching the
// rest of the runtime (camera, physics, scene). A terrain of cols x rows
// *vertices* spans (cols-1)*cellSize by (rows-1)*cellSize world units from
// its origin.
#pragma once

#include "pv/pv_handle.h"
#include "pv/pv_math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pv {

// Vertex format for generated terrain geometry. Deliberately mirrors
// Vertex3D in pv_internal.h field-for-field so terrain_render.cpp can
// memcpy-convert without this header knowing what a Vertex3D is.
struct TerrainVertex {
    float pos[3];
    float normal[3];
    float uv[2];
};

// A splat layer. Weight is a product of a height band and a slope band,
// each with soft edges, so layers cross-fade instead of stair-stepping.
// `blend` is the width of that soft edge, in the same units as the band
// it softens (world units for height, degrees for slope).
struct TerrainLayer {
    float r = 1, g = 1, b = 1;
    float minHeight = -1e9f, maxHeight = 1e9f;
    float minSlopeDeg = 0.0f, maxSlopeDeg = 90.0f;
    float blend = 1.0f;
};

// Number of detail levels a chunk can be built at. LOD n uses a vertex
// stride of 2^n, so LOD 0 is full density and LOD 3 is every 8th vertex.
// Four levels covers a 64-cell chunk down to 8 quads a side, past which
// the saving stops mattering and the silhouette starts visibly popping.
inline constexpr int kTerrainLodLevels = 4;

struct TerrainChunk {
    int col0 = 0, row0 = 0;  // top-left vertex index of this chunk
    int cols = 0, rows = 0;  // vertex counts covered (inclusive of the shared seam)
    Vec3 center;             // world-space center, for LOD distance
    float radius = 0.0f;     // bounding-sphere radius, for LOD distance
    uint64_t mesh[kTerrainLodLevels] = {0, 0, 0, 0}; // GPU handles, filled by terrain_render.cpp
};

struct Terrain {
    int cols = 0, rows = 0;      // vertex counts, so cols>=2 && rows>=2
    float cellSize = 1.0f;       // world units between adjacent vertices
    Vec3 origin;                 // world position of vertex (0,0)
    std::vector<float> heights;  // cols*rows, world units, already scaled

    std::vector<TerrainLayer> layers;

    int chunkSize = 32;          // cells per chunk edge
    float lodDistance = 64.0f;   // distance at which LOD steps down one level
    std::vector<TerrainChunk> chunks;

    uint64_t material = 0;       // bound by DrawTerrain when non-zero
    uint64_t bakedTexture = 0;   // produced by BakeTerrainTexture
    bool collisionEnabled = false;

    // Cached extremes, refreshed by refreshTerrainBounds(). Layer height
    // bands are authored in world units, and a bake needs the range to
    // normalize UVs, so recomputing this on every sample would be wasteful.
    float minHeight = 0.0f, maxHeight = 0.0f;

    // Diagnostics, reset each frame by DrawTerrain.
    int chunksDrawnLastFrame = 0;
    int trianglesDrawnLastFrame = 0;
};

HandleTable<Terrain>& terrains();

// ---------------------------------------------------------------------
// Authoring
// ---------------------------------------------------------------------

// Rebuilds minHeight/maxHeight. Call after any bulk height edit.
void refreshTerrainBounds(Terrain& t);

// Rebuilds the chunk list from cols/rows/chunkSize. Does not touch GPU
// handles; terrain_render.cpp uploads meshes separately.
void rebuildTerrainChunks(Terrain& t);

// Fractional Brownian motion over value noise, in place. `roughness` is
// the amplitude ratio between successive octaves (0.5 is the classic
// choice; higher is craggier). Deterministic for a given seed, because an
// editor that scrubs a terrain seed back and forth must see the same
// landscape each time.
void generateTerrainHeights(Terrain& t, uint32_t seed, float roughness, float heightScale);

// ---------------------------------------------------------------------
// Sampling. World-space X/Z in, all of them safe outside the terrain
// (they clamp to the edge rather than reading out of bounds).
// ---------------------------------------------------------------------
float terrainHeightAt(const Terrain& t, float worldX, float worldZ);
Vec3 terrainNormalAt(const Terrain& t, float worldX, float worldZ);
float terrainSlopeDegAt(const Terrain& t, float worldX, float worldZ);

// Splat weights for every layer at a point, normalized to sum to 1 when
// any layer matches at all. Returns false (and leaves `out` zeroed) when
// no layer covers the point, which is what the bake treats as bare rock.
bool terrainLayerWeightsAt(const Terrain& t, float worldX, float worldZ, std::vector<float>& out);

struct TerrainRayHit {
    bool hit = false;
    Vec3 point;
    Vec3 normal;
    float distance = 0.0f;
};

// Marches the ray in fixed steps and refines the crossing by bisection.
// Robust for the shallow grazing angles that a camera-to-ground pick
// produces, which is where a single-step analytic solve tends to miss.
TerrainRayHit terrainRaycast(const Terrain& t, const Vec3& origin, const Vec3& dir, float maxDist);

// ---------------------------------------------------------------------
// Collision. physics.cpp calls this after each step for every body when
// any terrain has collisionEnabled, so terrain collision works with both
// the PhysX and builtin backends without either of them knowing terrain
// exists. Returns true when the point was below the surface, in which
// case outPos is lifted to it and outNormal is the surface normal.
// ---------------------------------------------------------------------
bool terrainResolvePoint(const Vec3& pos, float radius, Vec3& outPos, Vec3& outNormal);
bool anyTerrainCollisionEnabled();

// ---------------------------------------------------------------------
// Geometry generation. Produces an indexed triangle list for one chunk at
// one LOD, in world space. Returns false for a degenerate chunk.
// ---------------------------------------------------------------------
bool buildTerrainChunkGeometry(const Terrain& t, const TerrainChunk& chunk, int lod,
                               std::vector<TerrainVertex>& verts, std::vector<uint32_t>& indices);

// Which LOD a chunk should draw at, given the camera. Clamped to
// [0, kTerrainLodLevels-1].
int selectTerrainLod(const Terrain& t, const TerrainChunk& chunk, const Vec3& cameraPos);

// RGBA8 pixels for the splat bake, row-major, `resolution` square.
std::vector<uint8_t> bakeTerrainPixels(const Terrain& t, int resolution);

} // namespace pv
