// animation_gltf.cpp -- real keyframe import from .gltf/.glb.
//
// This closes the gap runtime/README.md used to describe as "LoadAnimation
// does not yet parse real keyframes ... it builds one small placeholder
// clip". It now parses every animation in the file, every channel of every
// animation, and the node hierarchy those channels target.
//
// What's imported:
//   * the full node hierarchy (name, parent, bind TRS), depth-first from
//     the file's scene roots so parents always precede children
//   * every animation, by name, with its real duration
//   * translation / rotation / scale channels
//   * all three glTF interpolation modes: LINEAR, STEP, CUBICSPLINE
//   * both quaternion storage layouts (float and the normalized-integer
//     variants permitted by KHR_mesh_quantization)
//
// What isn't: morph-target weight channels are read into the clip but have
// no renderer consumer, and joint-palette skinning is out of scope (see the
// scope note at the top of pv_anim.h).
#include "pv/pv_anim.h"
#include "pv/pv_internal.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// cgltf is already vendored and already used by mesh.cpp for static
// geometry. Only that translation unit defines the implementation, so this
// one just takes the declarations.
#include "cgltf.h"

namespace pv {

namespace {

Interpolation mapInterpolation(cgltf_interpolation_type t) {
    switch (t) {
        case cgltf_interpolation_type_step: return Interpolation::Step;
        case cgltf_interpolation_type_cubic_spline: return Interpolation::CubicSpline;
        case cgltf_interpolation_type_linear:
        default: return Interpolation::Linear;
    }
}

bool mapPath(cgltf_animation_path_type t, AnimPath& out) {
    switch (t) {
        case cgltf_animation_path_type_translation: out = AnimPath::Translation; return true;
        case cgltf_animation_path_type_rotation: out = AnimPath::Rotation; return true;
        case cgltf_animation_path_type_scale: out = AnimPath::Scale; return true;
        case cgltf_animation_path_type_weights: out = AnimPath::Weights; return true;
        default: return false; // cgltf_animation_path_type_invalid
    }
}

// Reads a node's local transform. glTF nodes carry *either* a full 4x4
// matrix *or* a TRS triple, never both -- exporters differ on which they
// emit (Blender writes TRS, some CAD pipelines write matrices), so both
// have to be handled or half the files in the wild import wrong.
Transform readNodeTransform(const cgltf_node& n) {
    Transform t;
    if (n.has_matrix) {
        Mat4 m;
        std::memcpy(&m.m[0][0], n.matrix, sizeof(float) * 16); // glTF matrices are column-major, same as Mat4
        Vec3 s;
        decomposeTRS(m, t.position, t.rotation, s);
        t.scale = s;
        return t;
    }
    if (n.has_translation) t.position = Vec3(n.translation[0], n.translation[1], n.translation[2]);
    if (n.has_rotation) t.rotation = Quat(n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]);
    if (n.has_scale) t.scale = Vec3(n.scale[0], n.scale[1], n.scale[2]);
    return t;
}

// Depth-first flatten of the node graph, recording parent indices. Doing
// this depth-first (rather than iterating data->nodes in file order) is
// what guarantees composeHierarchy's parents-before-children precondition.
void flattenNodes(const cgltf_data* data, std::vector<AnimNode>& outNodes,
                  std::unordered_map<const cgltf_node*, int>& outIndex) {
    struct Walker {
        std::vector<AnimNode>& nodes;
        std::unordered_map<const cgltf_node*, int>& index;

        void visit(const cgltf_node* n, int parent, int depth) {
            // Cycle/depth guard: a hand-built or corrupt file can contain a
            // node loop, which would recurse until the stack dies.
            if (!n || depth > 256 || index.count(n)) return;
            int myIndex = static_cast<int>(nodes.size());
            index[n] = myIndex;
            AnimNode an;
            an.name = n->name ? n->name : ("node_" + std::to_string(myIndex));
            an.parent = parent;
            an.bindLocal = readNodeTransform(*n);
            nodes.push_back(std::move(an));
            for (cgltf_size i = 0; i < n->children_count; i++) {
                visit(n->children[i], myIndex, depth + 1);
            }
        }
    } walker{outNodes, outIndex};

    // Prefer the declared scene roots; fall back to every node without a
    // parent, because a surprising number of exported files declare no
    // scene at all.
    if (data->scene && data->scene->nodes_count > 0) {
        for (cgltf_size i = 0; i < data->scene->nodes_count; i++) walker.visit(data->scene->nodes[i], -1, 0);
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; i++) {
            if (data->nodes[i].parent == nullptr) walker.visit(&data->nodes[i], -1, 0);
        }
    }
    // Anything still unreached (a node referenced only by an animation
    // channel, which is legal) gets appended as a root so its channels
    // still have somewhere to land.
    for (cgltf_size i = 0; i < data->nodes_count; i++) {
        if (!outIndex.count(&data->nodes[i])) walker.visit(&data->nodes[i], -1, 0);
    }
}

} // namespace

// Returns false and fills `error` if the file can't be read or contains no
// animations at all. A file that parses but has zero animations is a real
// and common case (a static prop exported from Blender), and the caller
// reports it distinctly from a parse failure.
bool loadGltfAnimations(const std::string& path, AnimRecord& rec, std::string& error) {
    cgltf_options options{};
    cgltf_data* data = nullptr;

    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        error = "could not parse '" + path + "' as glTF/GLB";
        return false;
    }
    // Animation samplers live in buffer views, so the buffers have to be
    // resolved before any accessor can be unpacked. For .glb this is a
    // pointer fixup; for .gltf it loads the sidecar .bin relative to the
    // file, which is why the original path (not a normalized copy) is
    // passed through.
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        error = "could not load buffer data for '" + path + "' (is the .bin sidecar next to it?)";
        return false;
    }

    std::unordered_map<const cgltf_node*, int> nodeIndex;
    flattenNodes(data, rec.nodes, nodeIndex);

    if (data->animations_count == 0) {
        cgltf_free(data);
        error = "'" + path + "' contains no animations";
        return false;
    }

    rec.clips.clear();
    rec.clips.reserve(data->animations_count);

    for (cgltf_size ai = 0; ai < data->animations_count; ai++) {
        const cgltf_animation& ga = data->animations[ai];
        AnimClip clip;
        clip.name = ga.name ? ga.name : ("clip_" + std::to_string(ai));

        for (cgltf_size ci = 0; ci < ga.channels_count; ci++) {
            const cgltf_animation_channel& gc = ga.channels[ci];
            if (!gc.sampler || !gc.target_node) continue;

            AnimPath path;
            if (!mapPath(gc.target_path, path)) continue;

            auto it = nodeIndex.find(gc.target_node);
            if (it == nodeIndex.end()) continue;

            const cgltf_animation_sampler& gs = *gc.sampler;
            if (!gs.input || !gs.output) continue;

            AnimChannel ch;
            ch.targetNode = it->second;
            ch.path = path;
            ch.interp = mapInterpolation(gs.interpolation);

            // --- times ---
            const cgltf_size keyCount = gs.input->count;
            if (keyCount == 0) continue;
            ch.times.resize(keyCount);
            cgltf_accessor_unpack_floats(gs.input, ch.times.data(), keyCount);

            // --- values ---
            // cgltf_accessor_unpack_floats normalizes integer-quantized data
            // for us, which is what makes KHR_mesh_quantization files (and
            // Draco-decoded ones) import correctly without a special case.
            const cgltf_size comps = cgltf_num_components(gs.output->type);
            const cgltf_size total = gs.output->count * comps;
            std::vector<float> raw(total);
            cgltf_accessor_unpack_floats(gs.output, raw.data(), total);

            const cgltf_size expectedPerKey = (ch.interp == Interpolation::CubicSpline) ? 3 : 1;
            const cgltf_size expectedValues = keyCount * expectedPerKey;

            if (path == AnimPath::Rotation) {
                if (comps < 4 || gs.output->count < expectedValues) continue;
                ch.quatValues.resize(gs.output->count);
                for (cgltf_size k = 0; k < gs.output->count; k++) {
                    const float* v = &raw[k * comps];
                    // glTF quaternions are (x, y, z, w) -- the same order
                    // pv::Quat uses, chosen for exactly this reason, so this
                    // is a straight copy with no component shuffle.
                    ch.quatValues[k] = Quat(v[0], v[1], v[2], v[3]);
                }
                // Only the value entries are unit quaternions; the tangent
                // entries of a cubic spline are derivatives and must NOT be
                // normalized (doing so is a subtle bug that makes spline
                // rotations drift).
                if (ch.interp != Interpolation::CubicSpline) {
                    for (auto& q : ch.quatValues) q = q.normalized();
                } else {
                    for (cgltf_size k = 1; k < ch.quatValues.size(); k += 3) {
                        ch.quatValues[k] = ch.quatValues[k].normalized();
                    }
                }
            } else if (path == AnimPath::Weights) {
                // Stored so the data isn't silently lost, but nothing
                // consumes it yet.
                ch.vec3Values.reserve(gs.output->count);
                for (cgltf_size k = 0; k < gs.output->count; k++) {
                    ch.vec3Values.push_back(Vec3(raw[k * comps], 0, 0));
                }
            } else {
                if (comps < 3 || gs.output->count < expectedValues) continue;
                ch.vec3Values.resize(gs.output->count);
                for (cgltf_size k = 0; k < gs.output->count; k++) {
                    const float* v = &raw[k * comps];
                    ch.vec3Values[k] = Vec3(v[0], v[1], v[2]);
                }
            }

            clip.channels.push_back(std::move(ch));
        }

        clip.recomputeDuration();
        if (!clip.channels.empty()) rec.clips.push_back(std::move(clip));
    }

    cgltf_free(data);

    if (rec.clips.empty()) {
        error = "'" + path + "' declares animations but none had usable channels";
        return false;
    }

    rec.sourcePath = path;
    rec.fromFile = true;
    rec.localPose.assign(rec.nodes.size(), Transform{});
    rec.modelPose.assign(rec.nodes.size(), Mat4::identity());
    for (size_t i = 0; i < rec.nodes.size(); i++) rec.localPose[i] = rec.nodes[i].bindLocal;
    composeHierarchy(rec.nodes, rec.localPose, rec.modelPose);
    error.clear();
    return true;
}

} // namespace pv
