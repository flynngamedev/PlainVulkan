// terrain_render.cpp -- the GPU half of the terrain system.
//
// Everything here needs a Vulkan device: uploading per-chunk LOD meshes,
// uploading the baked splat texture, loading a heightmap image, and the
// draw itself. The geometry, the pixels and the height data all come from
// terrain.cpp, which has no Vulkan in it at all -- so the interesting
// logic stays testable on a machine with no GPU and this file stays a
// thin, boring wrapper.
//
// Pv::DrawTerrain used to be a Tier 4 stub in draw3d.cpp that logged a
// warning and returned. It is implemented here now; runtime/README.md's
// tier table is updated to match.
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"
#include "pv/pv_terrain.h"

#include "stb_image.h" // implementation lives in texture.cpp

#include <algorithm>
#include <cstring>
#include <vector>

namespace pv {
namespace {

Terrain* lookupTerrain(const Value& handle) {
    if (!handle.isHandle()) return nullptr;
    return terrains().get(handle.asHandle().id);
}

// TerrainVertex and Vertex3D are the same three arrays in the same order.
// The assert keeps that a fact rather than an assumption: if either
// struct ever gains a field, this stops compiling instead of uploading
// garbage.
static_assert(sizeof(TerrainVertex) == sizeof(Vertex3D),
              "TerrainVertex must stay layout-compatible with Vertex3D");

std::vector<Vertex3D> toVertex3D(const std::vector<TerrainVertex>& in) {
    std::vector<Vertex3D> out(in.size());
    if (!in.empty()) std::memcpy(out.data(), in.data(), in.size() * sizeof(Vertex3D));
    return out;
}

} // namespace

namespace rt {

Value BuildTerrainMesh(const Value& terrain) {
    if (!requireInitialized("Pv::BuildTerrainMesh")) return Value(false);
    Terrain* t = lookupTerrain(terrain);
    if (!t) {
        logLine("ERROR", "Pv::BuildTerrainMesh: invalid terrain handle");
        return Value(false);
    }
    if (t->chunks.empty()) rebuildTerrainChunks(*t);

    // One wait for the whole rebuild rather than one per mesh: a 64-chunk
    // terrain at 4 LODs is 256 destroys, and vkDeviceWaitIdle each time
    // would turn a rebuild into a visible multi-second stall.
    auto& e = engine();
    vkDeviceWaitIdle(e.device);

    std::vector<TerrainVertex> verts;
    std::vector<uint32_t> indices;
    int built = 0;
    for (TerrainChunk& c : t->chunks) {
        for (int lod = 0; lod < kTerrainLodLevels; ++lod) {
            // Freeing the previous handle first matters on a rebuild after
            // a height edit: without it every regeneration leaks a full
            // set of chunk meshes, which on a large terrain is hundreds of
            // megabytes of device memory.
            if (c.mesh[lod] != 0) {
                if (GpuMesh* old = e.meshes.get(c.mesh[lod])) {
                    destroyMeshBuffers(*old);
                    e.meshes.remove(c.mesh[lod]);
                }
                c.mesh[lod] = 0;
            }
            if (!buildTerrainChunkGeometry(*t, c, lod, verts, indices)) continue;
            c.mesh[lod] = uploadMeshData(toVertex3D(verts), indices);
            ++built;
        }
    }
    logLine("INFO", "Pv::BuildTerrainMesh: " + std::to_string(t->chunks.size()) + " chunk(s), " +
                        std::to_string(built) + " LOD mesh(es)");
    return Value(true);
}

Value BakeTerrainTexture(const Value& terrain, const Value& resolution) {
    if (!requireInitialized("Pv::BakeTerrainTexture")) return Value::MakeHandle(0, "texture");
    Terrain* t = lookupTerrain(terrain);
    if (!t) {
        logLine("ERROR", "Pv::BakeTerrainTexture: invalid terrain handle");
        return Value::MakeHandle(0, "texture");
    }
    if (t->layers.empty()) {
        logLine("WARNING", "Pv::BakeTerrainTexture: no layers added -- the bake will be flat grey. "
                           "Call Pv::AddTerrainLayer first.");
    }
    int res = static_cast<int>(resolution.asInt());
    std::vector<uint8_t> px = bakeTerrainPixels(*t, res);
    int actual = static_cast<int>(std::lround(std::sqrt(static_cast<double>(px.size() / 4))));
    uint64_t id = uploadTextureRGBA8(px.data(), static_cast<uint32_t>(actual), static_cast<uint32_t>(actual));
    t->bakedTexture = id;
    return Value::MakeHandle(id, "texture");
}

Value SetTerrainMaterial(const Value& terrain, const Value& material) {
    Terrain* t = lookupTerrain(terrain);
    if (!t) return Value(false);
    t->material = material.isHandle() ? material.asHandle().id : 0;
    return Value(true);
}

Value LoadTerrainHeightmap(const Value& terrain, const Value& filepath) {
    Terrain* t = lookupTerrain(terrain);
    if (!t) {
        logLine("ERROR", "Pv::LoadTerrainHeightmap: invalid terrain handle");
        return Value(false);
    }
    std::string path = filepath.asString();
    int w = 0, h = 0, channels = 0;
    // Force one channel: a heightmap is greyscale by definition, and
    // asking stb to convert means a colour PNG exported from a terrain
    // tool still loads instead of erroring on the user.
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels, 1);
    if (!pixels) {
        logLine("ERROR", "Pv::LoadTerrainHeightmap: could not read '" + path + "'");
        return Value(false);
    }
    if (w < 2 || h < 2) {
        stbi_image_free(pixels);
        logLine("ERROR", "Pv::LoadTerrainHeightmap: image must be at least 2x2");
        return Value(false);
    }

    // The image defines the grid, so resize the terrain to match rather
    // than resampling into whatever size it happened to be created at.
    t->cols = w;
    t->rows = h;
    t->heights.assign(static_cast<size_t>(w) * h, 0.0f);
    // Height range comes from what the terrain was already generated or
    // created with; a fresh terrain has none, so default to 1 unit per
    // full-white pixel and let the script scale it with SetTerrainHeight
    // or by regenerating.
    float scale = (t->maxHeight > t->minHeight) ? (t->maxHeight - t->minHeight) : 1.0f;
    for (int i = 0; i < w * h; ++i) {
        t->heights[static_cast<size_t>(i)] = (static_cast<float>(pixels[i]) / 255.0f) * scale;
    }
    stbi_image_free(pixels);

    refreshTerrainBounds(*t);
    rebuildTerrainChunks(*t);
    logLine("INFO", "Pv::LoadTerrainHeightmap: loaded " + std::to_string(w) + "x" + std::to_string(h) +
                        " from '" + path + "'");
    return Value(true);
}

Value DrawTerrain(const Value& heightmap, const Value& scale) {
    auto& e = engine();
    Terrain* t = lookupTerrain(heightmap);
    if (!t) {
        logLine("ERROR", "Pv::DrawTerrain: expected a terrain handle from Pv::CreateTerrain");
        return Value();
    }
    if (!e.frameActive) return Value();

    float s = static_cast<float>(scale.asFloat());
    if (!(s > 0.0f)) s = 1.0f;
    Mat4 model = Mat4::scale(Vec3(s, s, s));

    t->chunksDrawnLastFrame = 0;
    t->trianglesDrawnLastFrame = 0;

    bool missingMesh = false;
    for (const TerrainChunk& c : t->chunks) {
        int lod = selectTerrainLod(*t, c, e.camPos);
        // Walk down to a level that actually got built. A chunk whose cell
        // count doesn't divide by the LOD stride has no mesh at that
        // level (buildTerrainChunkGeometry refuses it), and skipping the
        // chunk entirely would punch a visible hole in the ground.
        while (lod > 0 && c.mesh[lod] == 0) --lod;
        uint64_t mesh = c.mesh[lod];
        if (mesh == 0) {
            missingMesh = true;
            continue;
        }
        drawSceneMesh(mesh, model, t->material ? t->material : 0);
        ++t->chunksDrawnLastFrame;
        if (const GpuMesh* gm = e.meshes.get(mesh)) {
            t->trianglesDrawnLastFrame += static_cast<int>(gm->indexCount / 3);
        }
    }
    if (missingMesh) {
        logLine("WARNING", "Pv::DrawTerrain: some chunks have no uploaded mesh -- call "
                           "Pv::BuildTerrainMesh after creating or regenerating the terrain");
    }
    return Value();
}

} // namespace rt
} // namespace pv
