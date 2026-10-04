// draw3d.cpp -- Pv::DrawMesh / DrawMeshEx / DrawBillboard (real, lit,
// depth-tested rendering) plus Pv::DrawSkybox, which is a documented
// Tier 4 stub (see runtime/README.md) -- it logs a warning and returns
// rather than silently pretending to render something.
// Pv::DrawTerrain was the other stub here; it is implemented for real now
// in terrain_render.cpp.
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

#include <cstring>

namespace pv {

static void drawMeshWithTransform(uint64_t meshHandle, const Mat4& model, VkDescriptorSet matSet,
                                  const MaterialRecord* mat) {
    auto& e = engine();
    if (!e.frameActive) return;
    GpuMesh* mesh = e.meshes.get(meshHandle);
    if (!mesh) return;

    e.shadowDraws.push_back(ShadowDrawItem{meshHandle, model});

    VkPipeline pipeline = e.wireframe ? e.pipeline3DWireframe : e.pipeline3D;
    vkCmdBindPipeline(e.activeCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    VkDescriptorSet sets[2] = {e.lightsDescriptorSet, matSet ? matSet : e.blankMaterialSet};
    vkCmdBindDescriptorSets(e.activeCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e.pipeline3DLayout, 0, 2, sets, 0, nullptr);

    struct PushData {
        float model[16];
        float color[4];
        float pbr[4];
    } push{};
    memcpy(push.model, model.m, sizeof(push.model));
    if (mat) {
        push.color[0] = mat->colorR;
        push.color[1] = mat->colorG;
        push.color[2] = mat->colorB;
        push.color[3] = mat->colorA;
        push.pbr[0] = mat->roughness;
        push.pbr[1] = mat->metallic;
        push.pbr[2] = mat->emissiveIntensity;
        push.pbr[3] = mat->heightScale;
    } else {
        push.color[0] = push.color[1] = push.color[2] = push.color[3] = 1.0f;
        push.pbr[0] = 0.5f;
    }
    vkCmdPushConstants(e.activeCmd, e.pipeline3DLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(e.activeCmd, 0, 1, &mesh->vertexBuffer, &offset);
    if (mesh->indexBuffer) {
        vkCmdBindIndexBuffer(e.activeCmd, mesh->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(e.activeCmd, mesh->indexCount, 1, 0, 0, 0);
    } else {
        vkCmdDraw(e.activeCmd, mesh->vertexCount, 1, 0, 0);
    }
}

// Lazily-created 1x1 unit quad (centered at origin, facing +Z) reused by
// DrawBillboard -- billboarding is just this quad drawn with a basis
// matrix built from the camera's right/up vectors instead of Euler angles.
static uint64_t unitQuadMesh() {
    static uint64_t handle = 0;
    if (handle != 0) return handle;
    std::vector<Vertex3D> verts = {
        {{-0.5f, -0.5f, 0}, {0, 0, 1}, {0, 1}},
        {{0.5f, -0.5f, 0}, {0, 0, 1}, {1, 1}},
        {{0.5f, 0.5f, 0}, {0, 0, 1}, {1, 0}},
        {{-0.5f, 0.5f, 0}, {0, 0, 1}, {0, 0}},
    };
    std::vector<uint32_t> idx = {0, 1, 2, 0, 2, 3};
    handle = uploadMeshData(verts, idx);
    return handle;
}

// Draws a mesh with an explicit model matrix and an optional material,
// which is what the scene renderer needs (it already has a world matrix and
// doesn't want to decompose it back into the position/euler/uniform-scale
// arguments DrawMeshEx takes).
//
// This also closes the material gap runtime/README.md described: the old
// DrawMesh path always bound the default white texture because the Pv::
// command reference has no "assign this material to that mesh" call. A
// scene entity carries its material directly, so here it can be honoured.
void drawSceneMesh(uint64_t meshHandle, const Mat4& model, uint64_t materialHandle) {
    auto& e = engine();
    const MaterialRecord* mat = nullptr;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (materialHandle != 0) {
        mat = e.materials.get(materialHandle);
        if (mat) {
            if (mat->materialSet) {
                set = mat->materialSet;
            } else {
                auto texView = [&](uint64_t h, VkImageView fallback) {
                    if (const GpuTexture* t = e.textures.get(h)) return t->view;
                    return fallback;
                };
                auto texSamp = [&](uint64_t h) {
                    if (const GpuTexture* t = e.textures.get(h)) return t->sampler;
                    return e.blankSampler;
                };
                set = allocMaterialDescriptorSet(texView(mat->albedoTexture, e.blankImageView),
                                                 texSamp(mat->albedoTexture),
                                                 texView(mat->normalTexture, e.blankImageView),
                                                 texSamp(mat->normalTexture),
                                                 texView(mat->heightTexture, e.blankImageView),
                                                 texSamp(mat->heightTexture));
                const_cast<MaterialRecord*>(mat)->materialSet = set;
            }
        }
    }
    drawMeshWithTransform(meshHandle, model, set, mat);
}

namespace rt {

Value DrawMesh(const Value& mesh, const Value& x, const Value& y, const Value& z) {
    Mat4 model = Mat4::translate(Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                                       static_cast<float>(z.asFloat())));
    drawMeshWithTransform(static_cast<uint64_t>(mesh.asInt()), model, VK_NULL_HANDLE, nullptr);
    return Value();
}

Value DrawMeshEx(const Value& mesh, const Value& x, const Value& y, const Value& z, const Value& rx, const Value& ry,
                  const Value& rz, const Value& scale) {
    Vec3 pos(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()), static_cast<float>(z.asFloat()));
    float s = static_cast<float>(scale.asFloat());
    // Rotation goes through the quaternion path now, but Quat::fromEuler is
    // defined to reproduce Mat4::rotateXYZ exactly (pv_math_tests.cpp checks
    // this), so existing scripts are unaffected.
    Mat4 model = Mat4::fromTRS(pos,
                               Quat::fromEuler(static_cast<float>(rx.asFloat()),
                                               static_cast<float>(ry.asFloat()),
                                               static_cast<float>(rz.asFloat())),
                               Vec3(s, s, s));
    drawMeshWithTransform(static_cast<uint64_t>(mesh.asInt()), model, VK_NULL_HANDLE, nullptr);
    return Value();
}

Value DrawSkybox(const Value& cubemap) {
    (void)cubemap;
    logLine("WARNING", "Pv::DrawSkybox: not implemented yet (Tier 4 gap, see runtime/README.md) -- no-op");
    return Value();
}

// Pv::DrawTerrain used to be a Tier 4 stub here. It is a real
// implementation now and lives in terrain_render.cpp, next to the rest of
// the terrain GPU code.

Value DrawBillboard(const Value& texture, const Value& x, const Value& y, const Value& z, const Value& w,
                     const Value& h) {
    auto& e = engine();
    Vec3 center(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()), static_cast<float>(z.asFloat()));
    // Camera-facing basis: the view matrix's rows are the world-space
    // right/up/forward axes of the camera (it's an orthonormal rotation),
    // so we can read them straight back out instead of re-deriving them.
    Mat4 view = buildViewMatrix();
    Vec3 right(view.m[0][0], view.m[1][0], view.m[2][0]);
    Vec3 up(view.m[0][1], view.m[1][1], view.m[2][1]);
    Vec3 fwd(view.m[0][2], view.m[1][2], view.m[2][2]);
    float W = static_cast<float>(w.asFloat()), H = static_cast<float>(h.asFloat());
    Mat4 model = Mat4::fromBasis(right * W, up * H, fwd, center);

    GpuTexture* tex = e.textures.get(static_cast<uint64_t>(texture.asInt()));
    VkDescriptorSet set = e.blankMaterialSet;
    if (tex) {
        if (!tex->pbrSet) {
            tex->pbrSet = allocMaterialDescriptorSet(tex->view, tex->sampler, e.blankImageView, e.blankSampler,
                                                     e.blankImageView, e.blankSampler);
        }
        set = tex->pbrSet;
    }
    drawMeshWithTransform(unitQuadMesh(), model, set, nullptr);
    return Value();
}

Value SetWireframe(const Value& enabled) {
    engine().wireframe = enabled.truthy();
    return Value();
}

} // namespace rt
} // namespace pv
