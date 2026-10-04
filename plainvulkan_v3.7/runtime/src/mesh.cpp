// mesh.cpp -- Pv::LoadMesh / CreateMesh / CreateCube / ... . Real .obj
// parsing via tinyobjloader and real .gltf/.glb parsing via cgltf (both
// vendored in runtime/third_party/); see the comment on loadGltf() for
// what's simplified about the glTF path.
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include <cmath>
#include <cstring>

namespace pv {

static uint64_t uploadMesh(const std::vector<Vertex3D>& verts, const std::vector<uint32_t>& indices) {
    auto& e = engine();
    GpuMesh mesh;
    mesh.vertexCount = static_cast<uint32_t>(verts.size());
    mesh.indexCount = static_cast<uint32_t>(indices.size());

    VkDeviceSize vSize = sizeof(Vertex3D) * verts.size();
    VkBuffer stagingV;
    VkDeviceMemory stagingVMem;
    createBuffer(vSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingV, stagingVMem);
    void* mapped;
    vkMapMemory(e.device, stagingVMem, 0, vSize, 0, &mapped);
    memcpy(mapped, verts.data(), static_cast<size_t>(vSize));
    vkUnmapMemory(e.device, stagingVMem);
    createBuffer(vSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mesh.vertexBuffer, mesh.vertexMemory);
    copyBuffer(stagingV, mesh.vertexBuffer, vSize);
    vkDestroyBuffer(e.device, stagingV, nullptr);
    vkFreeMemory(e.device, stagingVMem, nullptr);

    if (!indices.empty()) {
        VkDeviceSize iSize = sizeof(uint32_t) * indices.size();
        VkBuffer stagingI;
        VkDeviceMemory stagingIMem;
        createBuffer(iSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingI,
                     stagingIMem);
        vkMapMemory(e.device, stagingIMem, 0, iSize, 0, &mapped);
        memcpy(mapped, indices.data(), static_cast<size_t>(iSize));
        vkUnmapMemory(e.device, stagingIMem);
        createBuffer(iSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mesh.indexBuffer, mesh.indexMemory);
        copyBuffer(stagingI, mesh.indexBuffer, iSize);
        vkDestroyBuffer(e.device, stagingI, nullptr);
        vkFreeMemory(e.device, stagingIMem, nullptr);
    }

    return e.meshes.add(mesh);
}

} // namespace pv

// Public wrapper (declared in pv_internal.h) so other translation units
// (currently just draw3d.cpp, for DrawBillboard's unit quad) can upload
// mesh data without duplicating this buffer-upload logic.
uint64_t pv::uploadMeshData(const std::vector<pv::Vertex3D>& verts, const std::vector<uint32_t>& indices) {
    return pv::uploadMesh(verts, indices);
}

// Shared by Pv::UnloadMesh and by terrain_render.cpp's chunk rebuild. See
// the note in pv_internal.h about why this doesn't wait on the device.
void pv::destroyMeshBuffers(pv::GpuMesh& mesh) {
    auto& e = pv::engine();
    if (mesh.vertexBuffer) vkDestroyBuffer(e.device, mesh.vertexBuffer, nullptr);
    if (mesh.vertexMemory) vkFreeMemory(e.device, mesh.vertexMemory, nullptr);
    if (mesh.indexBuffer) vkDestroyBuffer(e.device, mesh.indexBuffer, nullptr);
    if (mesh.indexMemory) vkFreeMemory(e.device, mesh.indexMemory, nullptr);
    mesh.vertexBuffer = VK_NULL_HANDLE;
    mesh.vertexMemory = VK_NULL_HANDLE;
    mesh.indexBuffer = VK_NULL_HANDLE;
    mesh.indexMemory = VK_NULL_HANDLE;
    mesh.indexCount = 0;
    mesh.vertexCount = 0;
}

namespace pv {

static bool loadObj(const std::string& path, std::vector<Vertex3D>& verts, std::vector<uint32_t>& indices) {
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;
    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, path.c_str())) {
        logLine("ERROR", "Pv::LoadMesh (.obj) failed for '" + path + "': " + err);
        return false;
    }
    for (auto& shape : shapes) {
        for (auto& idx : shape.mesh.indices) {
            Vertex3D v{};
            v.pos[0] = attrib.vertices[3 * idx.vertex_index + 0];
            v.pos[1] = attrib.vertices[3 * idx.vertex_index + 1];
            v.pos[2] = attrib.vertices[3 * idx.vertex_index + 2];
            if (idx.normal_index >= 0) {
                v.normal[0] = attrib.normals[3 * idx.normal_index + 0];
                v.normal[1] = attrib.normals[3 * idx.normal_index + 1];
                v.normal[2] = attrib.normals[3 * idx.normal_index + 2];
            } else {
                v.normal[0] = 0;
                v.normal[1] = 1;
                v.normal[2] = 0;
            }
            if (idx.texcoord_index >= 0) {
                v.uv[0] = attrib.texcoords[2 * idx.texcoord_index + 0];
                v.uv[1] = 1.0f - attrib.texcoords[2 * idx.texcoord_index + 1];
            } else {
                v.uv[0] = v.uv[1] = 0;
            }
            indices.push_back(static_cast<uint32_t>(verts.size()));
            verts.push_back(v);
        }
    }
    return true;
}

// Simplification (see runtime/README.md): loads the first mesh primitive
// of the first scene node's mesh with POSITION/NORMAL/TEXCOORD_0
// attributes -- real static-geometry glTF/GLB loading, but no node
// hierarchy, multi-mesh files, materials, or skinning/animation import
// (Pv::LoadAnimation is a separate, simpler keyframe format -- see
// animation.cpp).
static bool loadGltf(const std::string& path, std::vector<Vertex3D>& verts, std::vector<uint32_t>& indices) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        logLine("ERROR", "Pv::LoadMesh (.gltf/.glb) failed to parse '" + path + "'");
        return false;
    }
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        logLine("ERROR", "Pv::LoadMesh (.gltf/.glb) failed to load buffers for '" + path + "'");
        cgltf_free(data);
        return false;
    }
    if (data->meshes_count == 0 || data->meshes[0].primitives_count == 0) {
        logLine("ERROR", "Pv::LoadMesh: '" + path + "' has no mesh primitives");
        cgltf_free(data);
        return false;
    }
    cgltf_primitive& prim = data->meshes[0].primitives[0];

    const float* positions = nullptr;
    const float* normals = nullptr;
    const float* uvs = nullptr;
    cgltf_size vertexCount = 0;
    std::vector<float> posBuf, normBuf, uvBuf;

    for (cgltf_size i = 0; i < prim.attributes_count; i++) {
        cgltf_attribute& attr = prim.attributes[i];
        cgltf_accessor* acc = attr.data;
        if (attr.type == cgltf_attribute_type_position) {
            vertexCount = acc->count;
            posBuf.resize(vertexCount * 3);
            cgltf_accessor_unpack_floats(acc, posBuf.data(), vertexCount * 3);
            positions = posBuf.data();
        } else if (attr.type == cgltf_attribute_type_normal) {
            normBuf.resize(acc->count * 3);
            cgltf_accessor_unpack_floats(acc, normBuf.data(), acc->count * 3);
            normals = normBuf.data();
        } else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) {
            uvBuf.resize(acc->count * 2);
            cgltf_accessor_unpack_floats(acc, uvBuf.data(), acc->count * 2);
            uvs = uvBuf.data();
        }
    }
    if (!positions) {
        logLine("ERROR", "Pv::LoadMesh: '" + path + "' primitive has no POSITION attribute");
        cgltf_free(data);
        return false;
    }
    verts.resize(vertexCount);
    for (cgltf_size i = 0; i < vertexCount; i++) {
        verts[i].pos[0] = positions[i * 3 + 0];
        verts[i].pos[1] = positions[i * 3 + 1];
        verts[i].pos[2] = positions[i * 3 + 2];
        if (normals) {
            verts[i].normal[0] = normals[i * 3 + 0];
            verts[i].normal[1] = normals[i * 3 + 1];
            verts[i].normal[2] = normals[i * 3 + 2];
        } else {
            verts[i].normal[0] = 0;
            verts[i].normal[1] = 1;
            verts[i].normal[2] = 0;
        }
        if (uvs) {
            verts[i].uv[0] = uvs[i * 2 + 0];
            verts[i].uv[1] = uvs[i * 2 + 1];
        } else {
            verts[i].uv[0] = verts[i].uv[1] = 0;
        }
    }
    if (prim.indices) {
        indices.resize(prim.indices->count);
        for (cgltf_size i = 0; i < prim.indices->count; i++) {
            indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i));
        }
    } else {
        indices.resize(vertexCount);
        for (cgltf_size i = 0; i < vertexCount; i++) indices[i] = static_cast<uint32_t>(i);
    }
    cgltf_free(data);
    return true;
}

static std::string extensionOf(const std::string& path) {
    auto pos = path.find_last_of('.');
    if (pos == std::string::npos) return "";
    std::string ext = path.substr(pos + 1);
    for (auto& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return ext;
}

namespace rt {

Value LoadMesh(const Value& filepath) {
    if (!requireInitialized("Pv::LoadMesh")) return Value::MakeHandle(0, "mesh");
    std::string path = filepath.asString();
    std::string ext = extensionOf(path);
    std::vector<Vertex3D> verts;
    std::vector<uint32_t> indices;
    bool ok = false;
    if (ext == "obj") {
        ok = loadObj(path, verts, indices);
    } else if (ext == "gltf" || ext == "glb") {
        ok = loadGltf(path, verts, indices);
    } else {
        logLine("ERROR", "Pv::LoadMesh: unsupported extension '." + ext + "' (supported: .obj .gltf .glb)");
    }
    if (!ok || verts.empty()) return Value::MakeHandle(0, "mesh");
    return Value::MakeHandle(uploadMesh(verts, indices), "mesh");
}

Value UnloadMesh(const Value& mesh) {
    auto& e = engine();
    uint64_t id = static_cast<uint64_t>(mesh.asInt());
    if (GpuMesh* m = e.meshes.get(id)) {
        vkDeviceWaitIdle(e.device);
        destroyMeshBuffers(*m);
        e.meshes.remove(id);
    }
    return Value();
}

// `vertices`/`indices` are PlainVulkan arrays. Each vertex element is
// itself expected to be an array: [x,y,z, nx,ny,nz, u,v] (8 numbers).
Value CreateMesh(const Value& vertices, const Value& indices) {
    if (!requireInitialized("Pv::CreateMesh")) return Value::MakeHandle(0, "mesh");
    std::vector<Vertex3D> verts;
    if (vertices.isArray()) {
        for (auto& item : const_cast<Value&>(vertices).arrayRef()) {
            if (!item.isArray()) continue;
            auto& a = const_cast<Value&>(item).arrayRef();
            Vertex3D v{};
            for (size_t i = 0; i < a.size() && i < 8; i++) {
                float f = static_cast<float>(a[i].asFloat());
                if (i < 3) v.pos[i] = f;
                else if (i < 6) v.normal[i - 3] = f;
                else v.uv[i - 6] = f;
            }
            verts.push_back(v);
        }
    }
    std::vector<uint32_t> idx;
    if (indices.isArray()) {
        for (auto& item : const_cast<Value&>(indices).arrayRef()) idx.push_back(static_cast<uint32_t>(item.asInt()));
    }
    if (verts.empty()) return Value::MakeHandle(0, "mesh");
    return Value::MakeHandle(uploadMesh(verts, idx), "mesh");
}

Value UpdateMesh(const Value& mesh, const Value& vertices) {
    (void)mesh;
    (void)vertices;
    logLine("WARNING", "Pv::UpdateMesh: dynamic mesh updates aren't implemented yet (Tier 4 gap, see "
                        "runtime/README.md); use Pv::UnloadMesh + Pv::CreateMesh to replace mesh data instead");
    return Value();
}

Value CreateCube(const Value& size) {
    if (!requireInitialized("Pv::CreateCube")) return Value::MakeHandle(0, "mesh");
    float s = static_cast<float>(size.asFloat()) * 0.5f;
    struct Face {
        float nx, ny, nz;
        float corners[4][3];
    };
    Face faces[6] = {
        {0, 0, 1, {{-s, -s, s}, {s, -s, s}, {s, s, s}, {-s, s, s}}},
        {0, 0, -1, {{s, -s, -s}, {-s, -s, -s}, {-s, s, -s}, {s, s, -s}}},
        {0, 1, 0, {{-s, s, s}, {s, s, s}, {s, s, -s}, {-s, s, -s}}},
        {0, -1, 0, {{-s, -s, -s}, {s, -s, -s}, {s, -s, s}, {-s, -s, s}}},
        {1, 0, 0, {{s, -s, s}, {s, -s, -s}, {s, s, -s}, {s, s, s}}},
        {-1, 0, 0, {{-s, -s, -s}, {-s, -s, s}, {-s, s, s}, {-s, s, -s}}},
    };
    std::vector<Vertex3D> verts;
    std::vector<uint32_t> idx;
    float uvs[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (auto& f : faces) {
        uint32_t base = static_cast<uint32_t>(verts.size());
        for (int i = 0; i < 4; i++) {
            Vertex3D v{};
            v.pos[0] = f.corners[i][0];
            v.pos[1] = f.corners[i][1];
            v.pos[2] = f.corners[i][2];
            v.normal[0] = f.nx;
            v.normal[1] = f.ny;
            v.normal[2] = f.nz;
            v.uv[0] = uvs[i][0];
            v.uv[1] = uvs[i][1];
            verts.push_back(v);
        }
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return Value::MakeHandle(uploadMesh(verts, idx), "mesh");
}

Value CreateSphere(const Value& radius, const Value& slices) {
    if (!requireInitialized("Pv::CreateSphere")) return Value::MakeHandle(0, "mesh");
    float r = static_cast<float>(radius.asFloat());
    int seg = std::max(3, static_cast<int>(slices.asInt()));
    int rings = seg;
    std::vector<Vertex3D> verts;
    std::vector<uint32_t> idx;
    for (int y = 0; y <= rings; y++) {
        float v = static_cast<float>(y) / rings;
        float theta = v * 3.14159265f;
        for (int x = 0; x <= seg; x++) {
            float u = static_cast<float>(x) / seg;
            float phi = u * 2.0f * 3.14159265f;
            float nx = sinf(theta) * cosf(phi);
            float ny = cosf(theta);
            float nz = sinf(theta) * sinf(phi);
            Vertex3D vert{};
            vert.pos[0] = nx * r;
            vert.pos[1] = ny * r;
            vert.pos[2] = nz * r;
            vert.normal[0] = nx;
            vert.normal[1] = ny;
            vert.normal[2] = nz;
            vert.uv[0] = u;
            vert.uv[1] = v;
            verts.push_back(vert);
        }
    }
    int stride = seg + 1;
    for (int y = 0; y < rings; y++) {
        for (int x = 0; x < seg; x++) {
            uint32_t a = y * stride + x;
            uint32_t b = a + stride;
            idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
    return Value::MakeHandle(uploadMesh(verts, idx), "mesh");
}

Value CreatePlane(const Value& width, const Value& height) {
    if (!requireInitialized("Pv::CreatePlane")) return Value::MakeHandle(0, "mesh");
    float w = static_cast<float>(width.asFloat()) * 0.5f;
    float h = static_cast<float>(height.asFloat()) * 0.5f;
    std::vector<Vertex3D> verts = {
        {{-w, 0, -h}, {0, 1, 0}, {0, 0}},
        {{w, 0, -h}, {0, 1, 0}, {1, 0}},
        {{w, 0, h}, {0, 1, 0}, {1, 1}},
        {{-w, 0, h}, {0, 1, 0}, {0, 1}},
    };
    std::vector<uint32_t> idx = {0, 1, 2, 0, 2, 3};
    return Value::MakeHandle(uploadMesh(verts, idx), "mesh");
}

Value CreateCylinder(const Value& radius, const Value& height) {
    if (!requireInitialized("Pv::CreateCylinder")) return Value::MakeHandle(0, "mesh");
    float r = static_cast<float>(radius.asFloat());
    float h = static_cast<float>(height.asFloat());
    const int seg = 24;
    std::vector<Vertex3D> verts;
    std::vector<uint32_t> idx;
    for (int i = 0; i <= seg; i++) {
        float a = (2.0f * 3.14159265f * i) / seg;
        float nx = cosf(a), nz = sinf(a);
        Vertex3D top{{nx * r, h * 0.5f, nz * r}, {nx, 0, nz}, {static_cast<float>(i) / seg, 0}};
        Vertex3D bot{{nx * r, -h * 0.5f, nz * r}, {nx, 0, nz}, {static_cast<float>(i) / seg, 1}};
        verts.push_back(top);
        verts.push_back(bot);
    }
    for (int i = 0; i < seg; i++) {
        uint32_t a = i * 2, b = a + 1, c = a + 2, d = a + 3;
        idx.insert(idx.end(), {a, b, c, c, b, d});
    }
    return Value::MakeHandle(uploadMesh(verts, idx), "mesh");
}

} // namespace rt
} // namespace pv
