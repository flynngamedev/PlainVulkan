// pv_anim.h -- the keyframe animation data model.
//
// This replaces the old placeholder, which stored a single yaw float per
// key and had no way to represent a real animation file. The model here is
// deliberately shaped like glTF's, because glTF is what LoadAnimation
// actually imports now (see animation_gltf.cpp):
//
//   clip -> channels -> (target node, target path, sampler)
//   sampler = keyframe times + values + an interpolation mode
//
// A "node" is a transform in the imported hierarchy -- a bone in a skinned
// character, or just the object root for a simple transform animation.
// Sampling a clip produces one local Transform per node; composing those
// down the parent chain produces the world pose the renderer consumes.
//
// Deliberate scope boundary, stated plainly: this animates *transforms*.
// Skinned vertex deformation (uploading a joint-matrix palette and
// skinning in the vertex shader) is a further step that needs the mesh
// importer to carry JOINTS_0/WEIGHTS_0 attributes, and is not done here.
// A clip whose channels target a single node -- which is what almost every
// prop, camera move, door, platform, and particle-emitter animation is --
// is fully and correctly animated end to end.
#pragma once

#include "pv/pv_handle.h"
#include "pv/pv_math.h"

#include <string>
#include <vector>

namespace pv {

// glTF's three sampler interpolation modes.
enum class Interpolation {
    Linear,     // default; lerp for vec3, nlerp/slerp for quaternions
    Step,       // hold the previous key -- used for visibility flags and hard cuts
    CubicSpline // Hermite with in/out tangents stored alongside each value
};

enum class AnimPath { Translation, Rotation, Scale, Weights };

// One animated property of one node. Times are seconds from clip start and
// are strictly increasing, which is what lets sampling binary-search.
struct AnimChannel {
    int targetNode = 0;
    AnimPath path = AnimPath::Translation;
    Interpolation interp = Interpolation::Linear;

    std::vector<float> times;

    // Only one of these is populated, per `path`. Storing them unpacked
    // (rather than a variant per key) keeps sampling a tight indexed read.
    // For CubicSpline, glTF stores three values per key -- in-tangent,
    // value, out-tangent -- so these hold 3x the entries of `times`, and
    // the sampler indexes accordingly.
    std::vector<Vec3> vec3Values;
    std::vector<Quat> quatValues;

    size_t keyCount() const { return times.size(); }
    bool empty() const { return times.empty(); }
};

// One named animation. `duration` is the largest time across all channels.
struct AnimClip {
    std::string name;
    float duration = 0.0f;
    std::vector<AnimChannel> channels;

    void recomputeDuration() {
        duration = 0.0f;
        for (const auto& c : channels) {
            if (!c.times.empty()) duration = std::max(duration, c.times.back());
        }
    }
};

// A node in the animated hierarchy. `parent` is an index into the owning
// AnimRecord's node vector, or -1 for a root.
struct AnimNode {
    std::string name;
    int parent = -1;
    Transform bindLocal; // the node's transform when no channel targets it
};

// One playing clip. Multiple layers active at once is what makes
// Pv::BlendAnimation real: each layer samples independently and the
// results are combined by weight.
struct AnimLayer {
    int clip = -1;
    float time = 0.0f;
    float speed = 1.0f;
    float weight = 1.0f;
    bool playing = false;
    bool looping = true;

    // Fade-in state, so switching clips doesn't pop. PlayAnimation sets a
    // fade on the outgoing layer rather than cutting it dead.
    float fadeRemaining = 0.0f;
    float fadeDuration = 0.0f;
    bool fadingOut = false;
};

struct AnimRecord {
    std::string sourcePath;
    std::vector<AnimClip> clips;
    std::vector<AnimNode> nodes;

    // Layer 0 is the primary clip driven by PlayAnimation; layers 1+ come
    // from BlendAnimation. Kept as a fixed small vector because a blend
    // tree deeper than a handful of layers isn't something the Pv:: API
    // can express anyway.
    static constexpr size_t MAX_LAYERS = 4;
    std::vector<AnimLayer> layers;

    // Output of the last UpdateAnimations: one local transform per node,
    // then the same composed to model space. Both are kept because the
    // editor's animation timeline wants to show local values while the
    // renderer wants world ones.
    std::vector<Transform> localPose;
    std::vector<Mat4> modelPose;

    // True once a clip has been sampled at least once, so DrawMeshEx knows
    // whether modelPose[0] is meaningful or still uninitialized.
    bool posed = false;

    // Whether the import found real keyframes. False means this record was
    // built by CreateAnimationClip from script, which is equally valid --
    // it just changes what the editor shows in the source column.
    bool fromFile = false;

    int findClip(const std::string& name) const {
        for (size_t i = 0; i < clips.size(); i++) {
            if (clips[i].name == name) return static_cast<int>(i);
        }
        return -1;
    }
    AnimLayer& primary() {
        if (layers.empty()) layers.push_back(AnimLayer{});
        return layers[0];
    }
};

// ------------------------------------------------------------------
// Sampling. Free functions rather than methods so the editor's timeline
// scrubber can evaluate a clip at an arbitrary time without disturbing
// any playback state.
// ------------------------------------------------------------------

// Evaluates one channel at `time`, writing into whichever component of
// `out` the channel's path targets. Times outside [0, duration] clamp.
void sampleChannel(const AnimChannel& ch, float time, Transform& out);

// Evaluates a whole clip into a per-node local pose. `out` is resized to
// nodeCount and pre-filled from `bindPose` so nodes with no channel keep
// their bind transform instead of collapsing to identity.
void sampleClip(const AnimClip& clip, float time, const std::vector<AnimNode>& nodes,
                std::vector<Transform>& out);

// Composes local transforms down the parent chain into model space.
// Assumes nodes are ordered parents-before-children, which the glTF
// importer guarantees by walking the scene graph depth-first.
void composeHierarchy(const std::vector<AnimNode>& nodes, const std::vector<Transform>& local,
                      std::vector<Mat4>& outModel);

// Weighted blend of two poses, in place into `a`. Rotations use slerp
// (not nlerp) because blend weights sweep across the full 0..1 range where
// nlerp's non-constant angular velocity becomes visible.
void blendPose(std::vector<Transform>& a, const std::vector<Transform>& b, float weight);

} // namespace pv
