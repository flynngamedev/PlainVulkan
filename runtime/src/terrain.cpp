// terrain.cpp -- the GPU-free half of the terrain system: storage,
// sampling, noise generation, LOD geometry, splat weights, raycast,
// collision, and every Pv:: terrain command that doesn't touch Vulkan.
//
// Keeping this file free of pv_internal.h is the point. It means
// runtime_tests can link it directly and assert on real heights, normals,
// slopes, LOD choices and ray hits without a GPU, a window, or a Vulkan
// loader -- which is the only way any of this gets tested in CI or in a
// container. terrain_render.cpp holds the four commands that genuinely
// need the device.
#include "pv/pv_runtime.h"
#include "pv/pv_terrain.h"
#include "pv/pv_value.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pv {

HandleTable<Terrain>& terrains() {
    static HandleTable<Terrain> table;
    return table;
}

namespace {

void terrainError(const char* fn, const std::string& msg) {
    std::fprintf(stderr, "[PlainVulkan][ERROR] %s: %s\n", fn, msg.c_str());
}

Terrain* lookup(const Value& handle) {
    if (!handle.isHandle()) return nullptr;
    return terrains().get(handle.asHandle().id);
}

// Grid height with clamped indices. Every sampler goes through this, so
// an out-of-range query reads the edge value instead of walking off the
// vector -- a terrain query with a camera outside the map is normal, not
// an error worth crashing over.
float heightAtIndex(const Terrain& t, int col, int row) {
    if (t.cols <= 0 || t.rows <= 0) return 0.0f;
    col = std::min(std::max(col, 0), t.cols - 1);
    row = std::min(std::max(row, 0), t.rows - 1);
    return t.heights[static_cast<size_t>(row) * t.cols + col];
}

// Hash-based value noise. A hash rather than a table because a table
// would have to be seeded and kept alive; this is stateless and gives the
// same value for the same (x, y, seed) forever.
float valueNoise2D(int x, int y, uint32_t seed) {
    uint32_t h = seed;
    h ^= static_cast<uint32_t>(x) * 0x8DA6B343u;
    h ^= static_cast<uint32_t>(y) * 0xD8163841u;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000u); // [0,1)
}

// Smoothstep-interpolated value noise. Bilinear alone leaves visible
// creases along the lattice; the ease curve removes them for the cost of
// two multiplies.
float smoothNoise(float x, float y, uint32_t seed) {
    int xi = static_cast<int>(std::floor(x));
    int yi = static_cast<int>(std::floor(y));
    float fx = x - static_cast<float>(xi);
    float fy = y - static_cast<float>(yi);
    float ux = fx * fx * (3.0f - 2.0f * fx);
    float uy = fy * fy * (3.0f - 2.0f * fy);

    float n00 = valueNoise2D(xi, yi, seed);
    float n10 = valueNoise2D(xi + 1, yi, seed);
    float n01 = valueNoise2D(xi, yi + 1, seed);
    float n11 = valueNoise2D(xi + 1, yi + 1, seed);

    return lerpf(lerpf(n00, n10, ux), lerpf(n01, n11, ux), uy);
}

// One-sided soft band: 1 inside [lo, hi], easing to 0 across `blend` on
// each side. Used for both the height and slope terms of a layer weight.
float softBand(float v, float lo, float hi, float blend) {
    if (blend <= 1e-6f) return (v >= lo && v <= hi) ? 1.0f : 0.0f;
    if (v < lo) return clampf(1.0f - (lo - v) / blend, 0.0f, 1.0f);
    if (v > hi) return clampf(1.0f - (v - hi) / blend, 0.0f, 1.0f);
    return 1.0f;
}

} // namespace

// ---------------------------------------------------------------------
// Authoring
// ---------------------------------------------------------------------

void refreshTerrainBounds(Terrain& t) {
    if (t.heights.empty()) {
        t.minHeight = t.maxHeight = 0.0f;
        return;
    }
    auto mm = std::minmax_element(t.heights.begin(), t.heights.end());
    t.minHeight = *mm.first;
    t.maxHeight = *mm.second;
}

void rebuildTerrainChunks(Terrain& t) {
    t.chunks.clear();
    if (t.cols < 2 || t.rows < 2) return;

    const int step = std::max(1, t.chunkSize);
    for (int row0 = 0; row0 + 1 < t.rows; row0 += step) {
        for (int col0 = 0; col0 + 1 < t.cols; col0 += step) {
            TerrainChunk c;
            c.col0 = col0;
            c.row0 = row0;
            // +1 so neighbouring chunks share their seam vertices; without
            // it every chunk boundary would be a one-cell hole.
            c.cols = std::min(step + 1, t.cols - col0);
            c.rows = std::min(step + 1, t.rows - row0);

            float minY = heightAtIndex(t, col0, row0);
            float maxY = minY;
            for (int r = 0; r < c.rows; ++r) {
                for (int q = 0; q < c.cols; ++q) {
                    float h = heightAtIndex(t, col0 + q, row0 + r);
                    minY = std::min(minY, h);
                    maxY = std::max(maxY, h);
                }
            }
            float halfW = (c.cols - 1) * t.cellSize * 0.5f;
            float halfD = (c.rows - 1) * t.cellSize * 0.5f;
            c.center = Vec3(t.origin.x + col0 * t.cellSize + halfW,
                            t.origin.y + (minY + maxY) * 0.5f,
                            t.origin.z + row0 * t.cellSize + halfD);
            float halfH = (maxY - minY) * 0.5f;
            c.radius = std::sqrt(halfW * halfW + halfD * halfD + halfH * halfH);
            t.chunks.push_back(c);
        }
    }
}

void generateTerrainHeights(Terrain& t, uint32_t seed, float roughness, float heightScale) {
    if (t.cols < 2 || t.rows < 2) return;
    t.heights.assign(static_cast<size_t>(t.cols) * t.rows, 0.0f);

    // Six octaves is where added detail stops being visible at typical
    // cell sizes; the loop also normalizes by the summed amplitude so
    // `roughness` changes the character of the terrain without changing
    // its overall height.
    const int octaves = 6;
    const float baseFrequency = 4.0f / static_cast<float>(std::max(t.cols, t.rows));
    float amplitudeSum = 0.0f;
    {
        float a = 1.0f;
        for (int o = 0; o < octaves; ++o) {
            amplitudeSum += a;
            a *= roughness;
        }
    }
    if (amplitudeSum <= 1e-6f) amplitudeSum = 1.0f;

    for (int row = 0; row < t.rows; ++row) {
        for (int col = 0; col < t.cols; ++col) {
            float freq = baseFrequency;
            float amp = 1.0f;
            float sum = 0.0f;
            for (int o = 0; o < octaves; ++o) {
                sum += smoothNoise(col * freq, row * freq, seed + static_cast<uint32_t>(o) * 0x9E3779B9u) * amp;
                freq *= 2.0f;
                amp *= roughness;
            }
            t.heights[static_cast<size_t>(row) * t.cols + col] = (sum / amplitudeSum) * heightScale;
        }
    }
    refreshTerrainBounds(t);
    rebuildTerrainChunks(t);
}

// ---------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------

float terrainHeightAt(const Terrain& t, float worldX, float worldZ) {
    if (t.cols < 2 || t.rows < 2 || t.cellSize <= 0.0f) return t.origin.y;

    float gx = (worldX - t.origin.x) / t.cellSize;
    float gz = (worldZ - t.origin.z) / t.cellSize;
    gx = clampf(gx, 0.0f, static_cast<float>(t.cols - 1));
    gz = clampf(gz, 0.0f, static_cast<float>(t.rows - 1));

    int col = static_cast<int>(gx);
    int row = static_cast<int>(gz);
    // Clamp the *base* index so the +1 lookups stay in range at the far
    // edge, where gx lands exactly on cols-1.
    col = std::min(col, t.cols - 2);
    row = std::min(row, t.rows - 2);
    float fx = gx - static_cast<float>(col);
    float fz = gz - static_cast<float>(row);

    float h00 = heightAtIndex(t, col, row);
    float h10 = heightAtIndex(t, col + 1, row);
    float h01 = heightAtIndex(t, col, row + 1);
    float h11 = heightAtIndex(t, col + 1, row + 1);

    return t.origin.y + lerpf(lerpf(h00, h10, fx), lerpf(h01, h11, fx), fz);
}

Vec3 terrainNormalAt(const Terrain& t, float worldX, float worldZ) {
    if (t.cols < 2 || t.rows < 2 || t.cellSize <= 0.0f) return Vec3(0, 1, 0);
    // Central differences one cell either side. Sampling the interpolated
    // surface rather than raw grid neighbours means the normal is
    // continuous across a cell, which matters for slope-based layer
    // blending far more than the extra two samples cost.
    const float d = t.cellSize;
    float hL = terrainHeightAt(t, worldX - d, worldZ);
    float hR = terrainHeightAt(t, worldX + d, worldZ);
    float hD = terrainHeightAt(t, worldX, worldZ - d);
    float hU = terrainHeightAt(t, worldX, worldZ + d);
    Vec3 n(hL - hR, 2.0f * d, hD - hU);
    return n.normalized();
}

float terrainSlopeDegAt(const Terrain& t, float worldX, float worldZ) {
    Vec3 n = terrainNormalAt(t, worldX, worldZ);
    float c = clampf(n.y, -1.0f, 1.0f);
    return std::acos(c) * 180.0f / PV_PI;
}

bool terrainLayerWeightsAt(const Terrain& t, float worldX, float worldZ, std::vector<float>& out) {
    out.assign(t.layers.size(), 0.0f);
    if (t.layers.empty()) return false;

    float h = terrainHeightAt(t, worldX, worldZ);
    float slope = terrainSlopeDegAt(t, worldX, worldZ);

    float total = 0.0f;
    for (size_t i = 0; i < t.layers.size(); ++i) {
        const TerrainLayer& L = t.layers[i];
        float w = softBand(h, L.minHeight, L.maxHeight, L.blend) *
                  softBand(slope, L.minSlopeDeg, L.maxSlopeDeg, L.blend);
        out[i] = w;
        total += w;
    }
    if (total <= 1e-6f) {
        std::fill(out.begin(), out.end(), 0.0f);
        return false;
    }
    for (float& w : out) w /= total;
    return true;
}

TerrainRayHit terrainRaycast(const Terrain& t, const Vec3& origin, const Vec3& dir, float maxDist) {
    TerrainRayHit hit;
    Vec3 d = dir.normalized();
    if (d.lengthSq() < 1e-12f || maxDist <= 0.0f) return hit;

    // Step at half a cell so a ray can't tunnel through a one-cell ridge,
    // then bisect the bracketing interval. Twenty bisections take the
    // residual error below a micrometre for any sane cell size, and it is
    // bounded work regardless of how grazing the ray is.
    const float step = std::max(t.cellSize * 0.5f, 1e-3f);
    float prevT = 0.0f;
    float prevDelta = origin.y - terrainHeightAt(t, origin.x, origin.z);
    if (prevDelta < 0.0f) return hit; // started underground: no surface crossing ahead

    for (float tt = step; tt <= maxDist; tt += step) {
        Vec3 p = origin + d * tt;
        float delta = p.y - terrainHeightAt(t, p.x, p.z);
        if (delta <= 0.0f) {
            float lo = prevT, hi = tt;
            for (int i = 0; i < 20; ++i) {
                float mid = (lo + hi) * 0.5f;
                Vec3 pm = origin + d * mid;
                if (pm.y - terrainHeightAt(t, pm.x, pm.z) > 0.0f) lo = mid;
                else hi = mid;
            }
            Vec3 ph = origin + d * hi;
            hit.hit = true;
            hit.point = ph;
            hit.normal = terrainNormalAt(t, ph.x, ph.z);
            hit.distance = hi;
            return hit;
        }
        prevT = tt;
        prevDelta = delta;
    }
    (void)prevDelta;
    return hit;
}

// ---------------------------------------------------------------------
// Collision
// ---------------------------------------------------------------------

bool anyTerrainCollisionEnabled() {
    for (const auto& kv : terrains()) {
        if (kv.second.collisionEnabled) return true;
    }
    return false;
}

bool terrainResolvePoint(const Vec3& pos, float radius, Vec3& outPos, Vec3& outNormal) {
    bool resolved = false;
    outPos = pos;
    outNormal = Vec3(0, 1, 0);
    for (auto& kv : terrains()) {
        Terrain& t = kv.second;
        if (!t.collisionEnabled || t.cols < 2 || t.rows < 2) continue;
        float surface = terrainHeightAt(t, outPos.x, outPos.z);
        float floorY = surface + radius;
        if (outPos.y < floorY) {
            outPos.y = floorY;
            outNormal = terrainNormalAt(t, outPos.x, outPos.z);
            resolved = true;
        }
    }
    return resolved;
}

// ---------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------

bool buildTerrainChunkGeometry(const Terrain& t, const TerrainChunk& chunk, int lod,
                               std::vector<TerrainVertex>& verts, std::vector<uint32_t>& indices) {
    verts.clear();
    indices.clear();
    if (t.cols < 2 || t.rows < 2 || chunk.cols < 2 || chunk.rows < 2) return false;

    lod = std::min(std::max(lod, 0), kTerrainLodLevels - 1);
    int stride = 1 << lod;

    // A chunk whose cell count doesn't divide by the stride would drop its
    // last row/column and leave a crack against the neighbour, so step
    // down until the stride fits. This is why LOD is chosen per chunk but
    // clamped here rather than trusted.
    while (stride > 1 && ((chunk.cols - 1) % stride != 0 || (chunk.rows - 1) % stride != 0)) {
        stride >>= 1;
    }

    const int vc = (chunk.cols - 1) / stride + 1;
    const int vr = (chunk.rows - 1) / stride + 1;
    if (vc < 2 || vr < 2) return false;

    verts.reserve(static_cast<size_t>(vc) * vr);
    for (int r = 0; r < vr; ++r) {
        for (int q = 0; q < vc; ++q) {
            int col = chunk.col0 + q * stride;
            int row = chunk.row0 + r * stride;
            float wx = t.origin.x + col * t.cellSize;
            float wz = t.origin.z + row * t.cellSize;
            float wy = t.origin.y + heightAtIndex(t, col, row);

            Vec3 n = terrainNormalAt(t, wx, wz);
            TerrainVertex v{};
            v.pos[0] = wx;
            v.pos[1] = wy;
            v.pos[2] = wz;
            v.normal[0] = n.x;
            v.normal[1] = n.y;
            v.normal[2] = n.z;
            // UVs span the whole terrain, not the chunk, so the baked
            // splat texture lines up across chunk seams.
            v.uv[0] = static_cast<float>(col) / static_cast<float>(t.cols - 1);
            v.uv[1] = static_cast<float>(row) / static_cast<float>(t.rows - 1);
            verts.push_back(v);
        }
    }

    indices.reserve(static_cast<size_t>(vc - 1) * (vr - 1) * 6);
    for (int r = 0; r + 1 < vr; ++r) {
        for (int q = 0; q + 1 < vc; ++q) {
            uint32_t i00 = static_cast<uint32_t>(r * vc + q);
            uint32_t i10 = i00 + 1;
            uint32_t i01 = static_cast<uint32_t>((r + 1) * vc + q);
            uint32_t i11 = i01 + 1;
            indices.push_back(i00);
            indices.push_back(i01);
            indices.push_back(i10);
            indices.push_back(i10);
            indices.push_back(i01);
            indices.push_back(i11);
        }
    }
    return true;
}

int selectTerrainLod(const Terrain& t, const TerrainChunk& chunk, const Vec3& cameraPos) {
    if (t.lodDistance <= 0.0f) return 0;
    // Distance to the chunk's bounding sphere, not its center: a large
    // chunk the camera is standing on top of should still be LOD 0.
    float d = (chunk.center - cameraPos).length() - chunk.radius;
    if (d <= 0.0f) return 0;
    int lod = static_cast<int>(d / t.lodDistance);
    return std::min(std::max(lod, 0), kTerrainLodLevels - 1);
}

std::vector<uint8_t> bakeTerrainPixels(const Terrain& t, int resolution) {
    resolution = std::min(std::max(resolution, 2), 4096);
    std::vector<uint8_t> px(static_cast<size_t>(resolution) * resolution * 4, 255);
    if (t.cols < 2 || t.rows < 2) return px;

    const float spanX = (t.cols - 1) * t.cellSize;
    const float spanZ = (t.rows - 1) * t.cellSize;
    std::vector<float> weights;

    for (int y = 0; y < resolution; ++y) {
        float v = static_cast<float>(y) / static_cast<float>(resolution - 1);
        float wz = t.origin.z + v * spanZ;
        for (int x = 0; x < resolution; ++x) {
            float u = static_cast<float>(x) / static_cast<float>(resolution - 1);
            float wx = t.origin.x + u * spanX;

            float r = 0.5f, g = 0.5f, b = 0.5f;
            if (terrainLayerWeightsAt(t, wx, wz, weights)) {
                r = g = b = 0.0f;
                for (size_t i = 0; i < weights.size(); ++i) {
                    r += t.layers[i].r * weights[i];
                    g += t.layers[i].g * weights[i];
                    b += t.layers[i].b * weights[i];
                }
            }
            size_t o = (static_cast<size_t>(y) * resolution + x) * 4;
            px[o + 0] = static_cast<uint8_t>(clampf(r, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[o + 1] = static_cast<uint8_t>(clampf(g, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[o + 2] = static_cast<uint8_t>(clampf(b, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[o + 3] = 255;
        }
    }
    return px;
}

// =====================================================================
// Pv:: commands
// =====================================================================
namespace rt {

Value CreateTerrain(const Value& cols, const Value& rows, const Value& cellSize) {
    Terrain t;
    t.cols = static_cast<int>(cols.asInt());
    t.rows = static_cast<int>(rows.asInt());
    t.cellSize = static_cast<float>(cellSize.asFloat());
    if (t.cols < 2 || t.rows < 2) {
        terrainError("Pv::CreateTerrain", "cols and rows must each be at least 2 (a terrain needs one cell)");
        return Value::MakeHandle(0, "terrain");
    }
    // A 4096x4096 grid is 16M vertices -- past the point where a single
    // terrain is the right structure, and enough allocation to be worth
    // refusing with a message rather than discovering as a bad_alloc.
    if (static_cast<int64_t>(t.cols) * t.rows > 16u * 1024u * 1024u) {
        terrainError("Pv::CreateTerrain", "cols*rows exceeds 16M vertices; split the world into several terrains");
        return Value::MakeHandle(0, "terrain");
    }
    if (!(t.cellSize > 0.0f)) {
        terrainError("Pv::CreateTerrain", "cellSize must be greater than zero");
        return Value::MakeHandle(0, "terrain");
    }
    t.heights.assign(static_cast<size_t>(t.cols) * t.rows, 0.0f);
    refreshTerrainBounds(t);
    rebuildTerrainChunks(t);
    return Value::MakeHandle(terrains().add(std::move(t)), "terrain");
}

Value GenerateTerrain(const Value& terrain, const Value& seed, const Value& roughness, const Value& heightScale) {
    Terrain* t = lookup(terrain);
    if (!t) {
        terrainError("Pv::GenerateTerrain", "invalid terrain handle");
        return Value(false);
    }
    float rough = clampf(static_cast<float>(roughness.asFloat()), 0.0f, 1.0f);
    generateTerrainHeights(*t, static_cast<uint32_t>(seed.asInt()), rough,
                           static_cast<float>(heightScale.asFloat()));
    return Value(true);
}

Value DestroyTerrain(const Value& terrain) {
    if (!terrain.isHandle()) return Value(false);
    terrains().remove(terrain.asHandle().id);
    return Value(true);
}

Value SetTerrainOrigin(const Value& terrain, const Value& x, const Value& y, const Value& z) {
    Terrain* t = lookup(terrain);
    if (!t) return Value(false);
    t->origin = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                     static_cast<float>(z.asFloat()));
    rebuildTerrainChunks(*t);
    return Value(true);
}

Value SetTerrainHeight(const Value& terrain, const Value& col, const Value& row, const Value& height) {
    Terrain* t = lookup(terrain);
    if (!t) return Value(false);
    int c = static_cast<int>(col.asInt());
    int r = static_cast<int>(row.asInt());
    if (c < 0 || r < 0 || c >= t->cols || r >= t->rows) {
        terrainError("Pv::SetTerrainHeight", "grid index out of range; the write was discarded");
        return Value(false);
    }
    t->heights[static_cast<size_t>(r) * t->cols + c] = static_cast<float>(height.asFloat());
    refreshTerrainBounds(*t);
    return Value(true);
}

Value GetTerrainHeight(const Value& terrain, const Value& x, const Value& z) {
    Terrain* t = lookup(terrain);
    if (!t) return Value(0.0);
    return Value(static_cast<double>(
        terrainHeightAt(*t, static_cast<float>(x.asFloat()), static_cast<float>(z.asFloat()))));
}

Value GetTerrainNormal(const Value& terrain, const Value& x, const Value& z) {
    Terrain* t = lookup(terrain);
    if (!t) return make_array({Value(0.0), Value(1.0), Value(0.0)});
    Vec3 n = terrainNormalAt(*t, static_cast<float>(x.asFloat()), static_cast<float>(z.asFloat()));
    return make_array({Value(static_cast<double>(n.x)), Value(static_cast<double>(n.y)),
                       Value(static_cast<double>(n.z))});
}

Value GetTerrainSlope(const Value& terrain, const Value& x, const Value& z) {
    Terrain* t = lookup(terrain);
    if (!t) return Value(0.0);
    return Value(static_cast<double>(
        terrainSlopeDegAt(*t, static_cast<float>(x.asFloat()), static_cast<float>(z.asFloat()))));
}

Value GetTerrainSize(const Value& terrain) {
    Value out;
    Terrain* t = lookup(terrain);
    if (!t) return out;
    member_ref(out, "cols") = Value(static_cast<int64_t>(t->cols));
    member_ref(out, "rows") = Value(static_cast<int64_t>(t->rows));
    member_ref(out, "cellSize") = Value(static_cast<double>(t->cellSize));
    member_ref(out, "width") = Value(static_cast<double>((t->cols - 1) * t->cellSize));
    member_ref(out, "depth") = Value(static_cast<double>((t->rows - 1) * t->cellSize));
    member_ref(out, "minHeight") = Value(static_cast<double>(t->minHeight));
    member_ref(out, "maxHeight") = Value(static_cast<double>(t->maxHeight));
    return out;
}

Value TerrainRaycast(const Value& terrain, const Value& ox, const Value& oy, const Value& oz, const Value& dx,
                     const Value& dy, const Value& dz, const Value& maxDist) {
    Value out;
    member_ref(out, "hit") = Value(false);
    Terrain* t = lookup(terrain);
    if (!t) return out;

    TerrainRayHit h = terrainRaycast(
        *t,
        Vec3(static_cast<float>(ox.asFloat()), static_cast<float>(oy.asFloat()), static_cast<float>(oz.asFloat())),
        Vec3(static_cast<float>(dx.asFloat()), static_cast<float>(dy.asFloat()), static_cast<float>(dz.asFloat())),
        static_cast<float>(maxDist.asFloat()));

    member_ref(out, "hit") = Value(h.hit);
    if (h.hit) {
        member_ref(out, "point") = make_array({Value(static_cast<double>(h.point.x)),
                                               Value(static_cast<double>(h.point.y)),
                                               Value(static_cast<double>(h.point.z))});
        member_ref(out, "normal") = make_array({Value(static_cast<double>(h.normal.x)),
                                                Value(static_cast<double>(h.normal.y)),
                                                Value(static_cast<double>(h.normal.z))});
        member_ref(out, "distance") = Value(static_cast<double>(h.distance));
    }
    return out;
}

Value AddTerrainLayer(const Value& terrain, const Value& r, const Value& g, const Value& b, const Value& minHeight,
                      const Value& maxHeight, const Value& minSlope, const Value& maxSlope) {
    Terrain* t = lookup(terrain);
    if (!t) return Value(static_cast<int64_t>(-1));
    TerrainLayer L;
    L.r = static_cast<float>(r.asFloat());
    L.g = static_cast<float>(g.asFloat());
    L.b = static_cast<float>(b.asFloat());
    L.minHeight = static_cast<float>(minHeight.asFloat());
    L.maxHeight = static_cast<float>(maxHeight.asFloat());
    L.minSlopeDeg = static_cast<float>(minSlope.asFloat());
    L.maxSlopeDeg = static_cast<float>(maxSlope.asFloat());
    t->layers.push_back(L);
    return Value(static_cast<int64_t>(t->layers.size() - 1));
}

Value SetTerrainLayerBlend(const Value& terrain, const Value& layer, const Value& blend) {
    Terrain* t = lookup(terrain);
    if (!t) return Value(false);
    int64_t i = layer.asInt();
    if (i < 0 || static_cast<size_t>(i) >= t->layers.size()) {
        terrainError("Pv::SetTerrainLayerBlend", "layer index out of range");
        return Value(false);
    }
    t->layers[static_cast<size_t>(i)].blend = std::max(0.0f, static_cast<float>(blend.asFloat()));
    return Value(true);
}

Value ClearTerrainLayers(const Value& terrain) {
    Terrain* t = lookup(terrain);
    if (!t) return Value(false);
    t->layers.clear();
    return Value(true);
}

Value SampleTerrainLayer(const Value& terrain, const Value& x, const Value& z, const Value& layer) {
    Terrain* t = lookup(terrain);
    if (!t) return Value(0.0);
    int64_t i = layer.asInt();
    if (i < 0 || static_cast<size_t>(i) >= t->layers.size()) return Value(0.0);
    std::vector<float> w;
    if (!terrainLayerWeightsAt(*t, static_cast<float>(x.asFloat()), static_cast<float>(z.asFloat()), w)) {
        return Value(0.0);
    }
    return Value(static_cast<double>(w[static_cast<size_t>(i)]));
}

Value SetTerrainLOD(const Value& terrain, const Value& chunkSize, const Value& lodDistance) {
    Terrain* t = lookup(terrain);
    if (!t) return Value(false);
    int cs = static_cast<int>(chunkSize.asInt());
    // Powers of two only: buildTerrainChunkGeometry halves the vertex
    // stride per LOD, and a chunk edge that isn't a power of two forces it
    // back down to LOD 0 for every chunk, silently disabling LOD.
    if (cs < 2 || (cs & (cs - 1)) != 0) {
        terrainError("Pv::SetTerrainLOD", "chunkSize must be a power of two and at least 2");
        return Value(false);
    }
    t->chunkSize = cs;
    t->lodDistance = static_cast<float>(lodDistance.asFloat());
    rebuildTerrainChunks(*t);
    return Value(true);
}

Value SetTerrainCollision(const Value& terrain, const Value& enabled) {
    Terrain* t = lookup(terrain);
    if (!t) return Value(false);
    t->collisionEnabled = enabled.truthy();
    return Value(true);
}

Value GetTerrainStats(const Value& terrain) {
    Value out;
    Terrain* t = lookup(terrain);
    if (!t) return out;
    int64_t verts = 0, tris = 0;
    for (const TerrainChunk& c : t->chunks) {
        verts += static_cast<int64_t>(c.cols) * c.rows;
        tris += static_cast<int64_t>(c.cols - 1) * (c.rows - 1) * 2;
    }
    member_ref(out, "chunks") = Value(static_cast<int64_t>(t->chunks.size()));
    member_ref(out, "chunkSize") = Value(static_cast<int64_t>(t->chunkSize));
    member_ref(out, "lodLevels") = Value(static_cast<int64_t>(kTerrainLodLevels));
    member_ref(out, "lodDistance") = Value(static_cast<double>(t->lodDistance));
    member_ref(out, "verticesFullDetail") = Value(verts);
    member_ref(out, "trianglesFullDetail") = Value(tris);
    member_ref(out, "chunksDrawnLastFrame") = Value(static_cast<int64_t>(t->chunksDrawnLastFrame));
    member_ref(out, "trianglesDrawnLastFrame") = Value(static_cast<int64_t>(t->trianglesDrawnLastFrame));
    member_ref(out, "layers") = Value(static_cast<int64_t>(t->layers.size()));
    member_ref(out, "collision") = Value(t->collisionEnabled);
    return out;
}

} // namespace rt
} // namespace pv
