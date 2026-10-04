// animation.cpp -- the `Pv::` animation commands.
//
// This is the layer script code talks to. The two halves it sits on top of
// are anim_sample.cpp (keyframe evaluation, no engine dependency) and
// animation_gltf.cpp (real import from .gltf/.glb).
//
// What changed from the original: LoadAnimation used to log a warning and
// fabricate one hard-coded three-key clip, because nothing parsed real
// keyframes. It now imports every clip, channel, and node from the file.
// The playback state machine that was already real is kept, and extended
// with layered blending (BlendAnimation is no longer a tracked no-op) and
// cross-fading, plus a scripting API for building clips without any asset
// file at all.
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace pv {

// Defined in animation_gltf.cpp.
bool loadGltfAnimations(const std::string& path, AnimRecord& rec, std::string& error);

namespace {

AnimRecord* getAnim(const Value& v) {
    return engine().anims.get(static_cast<uint64_t>(v.asInt()));
}

// Ensures a record has a pose buffer sized to its node count. Called before
// any sampling, so a record built by the scripting API (which starts with
// zero nodes) gets a single implicit root node to animate.
void ensurePoseBuffers(AnimRecord& rec) {
    if (rec.nodes.empty()) {
        AnimNode root;
        root.name = "root";
        root.parent = -1;
        rec.nodes.push_back(root);
    }
    if (rec.localPose.size() != rec.nodes.size()) {
        rec.localPose.resize(rec.nodes.size());
        for (size_t i = 0; i < rec.nodes.size(); i++) rec.localPose[i] = rec.nodes[i].bindLocal;
    }
    if (rec.modelPose.size() != rec.nodes.size()) rec.modelPose.assign(rec.nodes.size(), Mat4::identity());
}

// Finds (or creates) a channel targeting a given node+path, so the
// scripting API's AddKeyframe calls accumulate into one channel instead of
// creating a new single-key channel per call.
AnimChannel& channelFor(AnimClip& clip, int node, AnimPath path) {
    for (auto& ch : clip.channels) {
        if (ch.targetNode == node && ch.path == path) return ch;
    }
    clip.channels.push_back(AnimChannel{});
    AnimChannel& ch = clip.channels.back();
    ch.targetNode = node;
    ch.path = path;
    return ch;
}

// Inserts a key keeping `times` sorted, because sampleChannel binary-searches
// it. Scripts add keys in whatever order they like; this makes that safe.
size_t insertKeyTime(AnimChannel& ch, float time) {
    size_t i = static_cast<size_t>(std::lower_bound(ch.times.begin(), ch.times.end(), time) - ch.times.begin());
    ch.times.insert(ch.times.begin() + static_cast<long>(i), time);
    return i;
}

// Advances one layer's clock, handling looping and one-shot completion.
void advanceLayer(AnimRecord& rec, AnimLayer& layer, float dt) {
    if (!layer.playing || layer.clip < 0 || static_cast<size_t>(layer.clip) >= rec.clips.size()) return;
    const AnimClip& clip = rec.clips[static_cast<size_t>(layer.clip)];
    layer.time += dt * layer.speed;

    if (clip.duration <= 1e-6f) {
        layer.time = 0.0f;
        return;
    }
    if (layer.time > clip.duration) {
        if (layer.looping) {
            layer.time = std::fmod(layer.time, clip.duration);
        } else {
            layer.time = clip.duration;
            layer.playing = false;
        }
    } else if (layer.time < 0.0f) {
        // Negative speed: scrub backwards, wrapping at the start.
        layer.time = layer.looping ? clip.duration + std::fmod(layer.time, clip.duration) : 0.0f;
        if (!layer.looping) layer.playing = false;
    }
}

// Evaluates every active layer and blends them by weight into rec.localPose,
// then composes the hierarchy into rec.modelPose.
void evaluateRecord(AnimRecord& rec) {
    ensurePoseBuffers(rec);

    // Base: the bind pose, so nodes untouched by any channel keep their
    // authored transform rather than snapping to identity.
    for (size_t i = 0; i < rec.nodes.size(); i++) rec.localPose[i] = rec.nodes[i].bindLocal;

    std::vector<Transform> scratch;
    bool any = false;
    float accumulatedWeight = 0.0f;

    for (auto& layer : rec.layers) {
        if (layer.clip < 0 || static_cast<size_t>(layer.clip) >= rec.clips.size()) continue;
        if (layer.weight <= 0.0001f) continue;
        if (!layer.playing && !any) {
            // A stopped layer still contributes its held pose if nothing
            // else is playing -- that's what makes StopAnimation leave the
            // model at its rest pose rather than blinking to bind.
        } else if (!layer.playing) {
            continue;
        }

        sampleClip(rec.clips[static_cast<size_t>(layer.clip)], layer.time, rec.nodes, scratch);
        if (!any) {
            rec.localPose = scratch;
            accumulatedWeight = layer.weight;
            any = true;
        } else {
            // Normalized incremental blend: each additional layer is mixed
            // in at its share of the running total, which makes the result
            // independent of layer order and correct for any number of
            // layers whose weights don't sum to 1.
            float total = accumulatedWeight + layer.weight;
            float t = total > 1e-6f ? (layer.weight / total) : 0.0f;
            blendPose(rec.localPose, scratch, t);
            accumulatedWeight = total;
        }
    }

    composeHierarchy(rec.nodes, rec.localPose, rec.modelPose);
    rec.posed = true;
}

} // namespace

// Exposed to draw3d.cpp / scene.cpp so an animated mesh can be drawn with
// its current root transform applied.
const Mat4* animRootMatrix(uint64_t animHandle) {
    AnimRecord* rec = engine().anims.get(animHandle);
    if (!rec || !rec->posed || rec->modelPose.empty()) return nullptr;
    return &rec->modelPose[0];
}

// Exposed so the scene system can drive an animator without going through
// the Value-boxed public API.
void animAdvance(AnimRecord& rec, float dt) {
    bool anyPlaying = false;
    for (auto& layer : rec.layers) {
        if (layer.fadeRemaining > 0.0f) {
            layer.fadeRemaining = std::max(0.0f, layer.fadeRemaining - dt);
            float k = layer.fadeDuration > 1e-6f ? (layer.fadeRemaining / layer.fadeDuration) : 0.0f;
            layer.weight = layer.fadingOut ? k : (1.0f - k);
            if (layer.fadeRemaining <= 0.0f && layer.fadingOut) layer.playing = false;
        }
        advanceLayer(rec, layer, dt);
        anyPlaying = anyPlaying || layer.playing;
    }
    // Re-evaluating even when nothing is playing keeps a paused or scrubbed
    // animation showing the pose at its current time, which is what the
    // editor's timeline needs.
    evaluateRecord(rec);
    (void)anyPlaying;
}

namespace rt {

Value LoadAnimation(const Value& filepath) {
    std::string path = filepath.asString();
    AnimRecord rec;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        logLine("WARNING", "Pv::LoadAnimation: '" + path +
                               "' not found -- returning an empty animation. Playback calls on it are "
                               "safe no-ops.");
        rec.sourcePath = path;
        ensurePoseBuffers(rec);
        rec.layers.push_back(AnimLayer{});
        return Value::MakeHandle(engine().anims.add(std::move(rec)), "animation");
    }

    std::string error;
    if (loadGltfAnimations(path, rec, error)) {
        logLine("INFO", "Pv::LoadAnimation: '" + path + "' -- " + std::to_string(rec.clips.size()) +
                            " clip(s), " + std::to_string(rec.nodes.size()) + " node(s)");
    } else {
        // Not fatal: the handle is still valid and every command on it is
        // safe. The message says exactly what went wrong rather than a
        // generic "not implemented".
        logLine("WARNING", "Pv::LoadAnimation: " + error);
        rec.sourcePath = path;
    }

    ensurePoseBuffers(rec);
    AnimLayer primary;
    if (!rec.clips.empty()) {
        primary.clip = 0;
        primary.weight = 1.0f;
    }
    rec.layers.push_back(primary);
    evaluateRecord(rec);

    return Value::MakeHandle(engine().anims.add(std::move(rec)), "animation");
}

Value UnloadAnimation(const Value& anim) {
    engine().anims.remove(static_cast<uint64_t>(anim.asInt()));
    return Value();
}

Value PlayAnimation(const Value& anim, const Value& clipName) {
    AnimRecord* a = getAnim(anim);
    if (!a) return Value(false);
    int idx = a->findClip(clipName.asString());
    if (idx < 0) {
        // Empty string means "whatever's first", which is what a one-clip
        // file wants and saves the script from knowing the exporter's name.
        if (clipName.asString().empty() && !a->clips.empty()) {
            idx = 0;
        } else {
            logLine("WARNING", "Pv::PlayAnimation: no clip named '" + clipName.asString() + "'");
            return Value(false);
        }
    }
    AnimLayer& l = a->primary();
    l.clip = idx;
    l.time = 0.0f;
    l.playing = true;
    l.weight = 1.0f;
    l.fadeRemaining = 0.0f;
    l.fadingOut = false;
    // Any blend layers added earlier belong to the previous clip; dropping
    // them here is what makes PlayAnimation a clean restart.
    if (a->layers.size() > 1) a->layers.resize(1);
    evaluateRecord(*a);
    return Value(true);
}

Value PauseAnimation(const Value& anim) {
    if (auto* a = getAnim(anim)) {
        for (auto& l : a->layers) l.playing = false;
    }
    return Value();
}

Value StopAnimation(const Value& anim) {
    if (auto* a = getAnim(anim)) {
        for (auto& l : a->layers) {
            l.playing = false;
            l.time = 0.0f;
        }
        evaluateRecord(*a);
    }
    return Value();
}

Value SetAnimationSpeed(const Value& anim, const Value& speed) {
    if (auto* a = getAnim(anim)) a->primary().speed = static_cast<float>(speed.asFloat());
    return Value();
}

Value SetAnimationLoop(const Value& anim, const Value& loop_) {
    if (auto* a = getAnim(anim)) {
        for (auto& l : a->layers) l.looping = loop_.truthy();
    }
    return Value();
}

Value IsAnimationPlaying(const Value& anim) {
    auto* a = getAnim(anim);
    if (!a) return Value(false);
    for (const auto& l : a->layers) {
        if (l.playing) return Value(true);
    }
    return Value(false);
}

Value GetAnimationTime(const Value& anim) {
    auto* a = getAnim(anim);
    return Value(a ? static_cast<double>(a->primary().time) : 0.0);
}

Value SetAnimationTime(const Value& anim, const Value& time) {
    if (auto* a = getAnim(anim)) {
        a->primary().time = static_cast<float>(time.asFloat());
        // Re-evaluate immediately: scrubbing should update the visible pose
        // now, not on the next UpdateAnimations call.
        evaluateRecord(*a);
    }
    return Value();
}

Value GetAnimationDuration(const Value& anim) {
    auto* a = getAnim(anim);
    if (!a) return Value(0.0);
    int c = a->primary().clip;
    if (c < 0 || static_cast<size_t>(c) >= a->clips.size()) return Value(0.0);
    return Value(static_cast<double>(a->clips[static_cast<size_t>(c)].duration));
}

Value BlendAnimation(const Value& anim, const Value& clipA, const Value& weight) {
    AnimRecord* a = getAnim(anim);
    if (!a) return Value(false);
    int idx = a->findClip(clipA.asString());
    if (idx < 0) {
        logLine("WARNING", "Pv::BlendAnimation: no clip named '" + clipA.asString() + "'");
        return Value(false);
    }
    float w = clampf(static_cast<float>(weight.asFloat()), 0.0f, 1.0f);

    // Reuse an existing layer for this clip so calling BlendAnimation every
    // frame (the normal usage, driving a weight from input) adjusts one
    // layer instead of appending a new one until MAX_LAYERS is hit.
    for (size_t i = 1; i < a->layers.size(); i++) {
        if (a->layers[i].clip == idx) {
            a->layers[i].weight = w;
            a->layers[i].playing = w > 0.0001f;
            return Value(true);
        }
    }
    if (a->layers.size() >= AnimRecord::MAX_LAYERS) {
        logLine("WARNING", "Pv::BlendAnimation: already blending " +
                               std::to_string(AnimRecord::MAX_LAYERS) +
                               " clips; ignoring '" + clipA.asString() + "'");
        return Value(false);
    }
    AnimLayer l;
    l.clip = idx;
    l.weight = w;
    l.playing = w > 0.0001f;
    l.looping = a->primary().looping;
    l.speed = a->primary().speed;
    l.time = a->primary().time;
    a->layers.push_back(l);
    return Value(true);
}

Value UpdateAnimations(const Value& deltaTime) {
    float dt = static_cast<float>(deltaTime.asFloat());
    for (auto& kv : engine().anims) animAdvance(kv.second, dt);
    return Value();
}

// ---- new commands -----------------------------------------------------

Value CrossFadeAnimation(const Value& anim, const Value& clipName, const Value& fadeSeconds) {
    AnimRecord* a = getAnim(anim);
    if (!a) return Value(false);
    int idx = a->findClip(clipName.asString());
    if (idx < 0) {
        logLine("WARNING", "Pv::CrossFadeAnimation: no clip named '" + clipName.asString() + "'");
        return Value(false);
    }
    float fade = std::max(0.0f, static_cast<float>(fadeSeconds.asFloat()));
    if (fade <= 1e-4f) return PlayAnimation(anim, clipName);

    AnimLayer& cur = a->primary();
    if (cur.clip == idx && cur.playing) return Value(true); // already there

    // The outgoing clip becomes a fading-out blend layer while the new one
    // fades in as the primary. This is what makes a walk->run transition
    // continuous instead of a visible pop.
    AnimLayer outgoing = cur;
    outgoing.fadingOut = true;
    outgoing.fadeDuration = fade;
    outgoing.fadeRemaining = fade;

    cur.clip = idx;
    cur.time = 0.0f;
    cur.playing = true;
    cur.weight = 0.0f;
    cur.fadingOut = false;
    cur.fadeDuration = fade;
    cur.fadeRemaining = fade;

    if (a->layers.size() >= AnimRecord::MAX_LAYERS) a->layers.resize(AnimRecord::MAX_LAYERS - 1);
    if (outgoing.clip >= 0) a->layers.push_back(outgoing);
    return Value(true);
}

Value GetAnimationClipCount(const Value& anim) {
    auto* a = getAnim(anim);
    return Value(static_cast<int64_t>(a ? a->clips.size() : 0));
}

Value GetAnimationClipName(const Value& anim, const Value& index) {
    auto* a = getAnim(anim);
    if (!a) return Value("");
    int64_t i = index.asInt();
    if (i < 0 || static_cast<size_t>(i) >= a->clips.size()) return Value("");
    return Value(a->clips[static_cast<size_t>(i)].name);
}

// Returns the sampled root transform as an object, so script can drive
// anything at all from an animation curve -- a camera, a light's intensity,
// a UI element -- not only meshes.
Value GetAnimationTransform(const Value& anim) {
    Value out = Value::MakeObject();
    AnimRecord* a = getAnim(anim);
    if (!a || a->localPose.empty()) {
        member_ref(out, "valid") = Value(false);
        return out;
    }
    const Transform& t = a->localPose[0];
    Vec3 e = t.rotation.toEuler();
    member_ref(out, "valid") = Value(true);
    member_ref(out, "x") = Value(static_cast<double>(t.position.x));
    member_ref(out, "y") = Value(static_cast<double>(t.position.y));
    member_ref(out, "z") = Value(static_cast<double>(t.position.z));
    member_ref(out, "pitch") = Value(static_cast<double>(e.x));
    member_ref(out, "yaw") = Value(static_cast<double>(e.y));
    member_ref(out, "roll") = Value(static_cast<double>(e.z));
    member_ref(out, "scaleX") = Value(static_cast<double>(t.scale.x));
    member_ref(out, "scaleY") = Value(static_cast<double>(t.scale.y));
    member_ref(out, "scaleZ") = Value(static_cast<double>(t.scale.z));
    return out;
}

// ---- authoring a clip from script (no asset file needed) --------------

Value CreateAnimation() {
    AnimRecord rec;
    ensurePoseBuffers(rec);
    rec.layers.push_back(AnimLayer{});
    return Value::MakeHandle(engine().anims.add(std::move(rec)), "animation");
}

Value AddAnimationClip(const Value& anim, const Value& clipName) {
    AnimRecord* a = getAnim(anim);
    if (!a) return Value(-1);
    int existing = a->findClip(clipName.asString());
    if (existing >= 0) return Value(static_cast<int64_t>(existing));
    AnimClip clip;
    clip.name = clipName.asString();
    a->clips.push_back(std::move(clip));
    int idx = static_cast<int>(a->clips.size()) - 1;
    if (a->primary().clip < 0) a->primary().clip = idx;
    return Value(static_cast<int64_t>(idx));
}

// One entry point for all three channel kinds, keyed by a string, rather
// than three near-identical commands. `kind` is "position" | "rotation" |
// "scale"; rotation takes Euler angles in radians for consistency with
// RotateCamera and DrawMeshEx.
Value AddAnimationKey(const Value& anim, const Value& clipName, const Value& kind, const Value& time,
                      const Value& a1, const Value& a2, const Value& a3) {
    AnimRecord* a = getAnim(anim);
    if (!a) return Value(false);
    int ci = a->findClip(clipName.asString());
    if (ci < 0) {
        logLine("WARNING", "Pv::AddAnimationKey: no clip named '" + clipName.asString() +
                               "' -- call Pv::AddAnimationClip first");
        return Value(false);
    }
    ensurePoseBuffers(*a);

    std::string k = kind.asString();
    AnimPath path;
    if (k == "position" || k == "translation") path = AnimPath::Translation;
    else if (k == "rotation") path = AnimPath::Rotation;
    else if (k == "scale") path = AnimPath::Scale;
    else {
        logLine("WARNING", "Pv::AddAnimationKey: unknown key kind '" + k +
                               "' (expected \"position\", \"rotation\", or \"scale\")");
        return Value(false);
    }

    AnimClip& clip = a->clips[static_cast<size_t>(ci)];
    AnimChannel& ch = channelFor(clip, 0, path);
    float t = static_cast<float>(time.asFloat());
    size_t at = insertKeyTime(ch, t);

    float v1 = static_cast<float>(a1.asFloat());
    float v2 = static_cast<float>(a2.asFloat());
    float v3 = static_cast<float>(a3.asFloat());

    if (path == AnimPath::Rotation) {
        ch.quatValues.insert(ch.quatValues.begin() + static_cast<long>(at), Quat::fromEuler(v1, v2, v3));
    } else {
        ch.vec3Values.insert(ch.vec3Values.begin() + static_cast<long>(at), Vec3(v1, v2, v3));
    }
    clip.recomputeDuration();
    return Value(true);
}

} // namespace rt
} // namespace pv
