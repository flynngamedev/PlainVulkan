// anim_sample.cpp -- keyframe evaluation. Deliberately free of any Vulkan
// or Engine dependency so it can be unit-tested on its own (see
// tests/anim_tests.cpp) and so the editor's timeline scrubber can evaluate
// a clip at an arbitrary time without touching playback state.
#include "pv/pv_anim.h"

#include <algorithm>
#include <cmath>

namespace pv {

namespace {

// Finds the key index `i` such that times[i] <= t < times[i+1], and the
// normalized position of t within that span.
//
// Binary search rather than a linear scan from the start: a 30-second clip
// sampled at 60Hz has ~1800 keys per channel, and a linear scan makes
// sampling O(keys) per channel per frame. Callers that play forward could
// cache a cursor, but scrubbing (the editor) and blending (multiple layers
// at different times) both jump around, so an unconditional log-n search is
// the honest choice.
size_t findSpan(const std::vector<float>& times, float t, float& outLocalT) {
    const size_t n = times.size();
    if (n == 0) { outLocalT = 0.0f; return 0; }
    if (n == 1 || t <= times.front()) { outLocalT = 0.0f; return 0; }
    if (t >= times.back()) { outLocalT = 1.0f; return n - 2; }

    // upper_bound gives the first key strictly after t; the span starts at
    // the one before it.
    size_t hi = static_cast<size_t>(std::upper_bound(times.begin(), times.end(), t) - times.begin());
    size_t i = hi - 1;
    float t0 = times[i], t1 = times[i + 1];
    float dt = t1 - t0;
    // Duplicate timestamps are legal in glTF (they encode a hard cut).
    // Guarding the divide keeps that from becoming a NaN that propagates
    // into every downstream transform.
    outLocalT = dt > 1e-8f ? (t - t0) / dt : 0.0f;
    return i;
}

// glTF STEP semantics: the output is the value of the last key at or before
// `t`. findSpan can't answer this on its own -- at or past the final key it
// clamps to the *span* n-2 with localT == 1, which for a step channel would
// wrongly hold the second-to-last value forever. Resolving the index
// separately is what makes a step channel actually reach its final key.
size_t stepIndex(const std::vector<float>& times, float t) {
    const size_t n = times.size();
    if (n == 0) return 0;
    if (t >= times.back()) return n - 1;
    if (t <= times.front()) return 0;
    size_t hi = static_cast<size_t>(std::upper_bound(times.begin(), times.end(), t) - times.begin());
    return hi > 0 ? hi - 1 : 0;
}

// Cubic Hermite, the interpolation glTF's CUBICSPLINE mode specifies.
// Values are stored as triples (inTangent, value, outTangent) per key, and
// the tangents are scaled by the span duration -- omitting that scaling is
// the classic cause of "cubic-spline animations look right at one playback
// speed and overshoot wildly at another".
template <typename T>
T hermite(const T& v0, const T& outTangent0, const T& v1, const T& inTangent1, float t, float dt) {
    float t2 = t * t;
    float t3 = t2 * t;
    float h00 = 2 * t3 - 3 * t2 + 1;
    float h10 = t3 - 2 * t2 + t;
    float h01 = -2 * t3 + 3 * t2;
    float h11 = t3 - t2;
    return v0 * h00 + outTangent0 * (h10 * dt) + v1 * h01 + inTangent1 * (h11 * dt);
}

} // namespace

void sampleChannel(const AnimChannel& ch, float time, Transform& out) {
    if (ch.times.empty()) return;

    float localT = 0.0f;
    size_t i = findSpan(ch.times, time, localT);
    const bool cubic = (ch.interp == Interpolation::CubicSpline);
    const bool step = (ch.interp == Interpolation::Step);
    const size_t n = ch.times.size();

    if (ch.path == AnimPath::Rotation) {
        if (ch.quatValues.empty()) return;
        auto keyAt = [&](size_t k) -> Quat {
            // CUBICSPLINE stores 3 entries per key; the value is the middle one.
            size_t idx = cubic ? (k * 3 + 1) : k;
            return idx < ch.quatValues.size() ? ch.quatValues[idx] : Quat::identity();
        };
        if (step) {
            out.rotation = keyAt(stepIndex(ch.times, time));
            return;
        }
        if (n == 1) {
            out.rotation = keyAt(0);
            return;
        }
        if (cubic) {
            size_t base0 = i * 3, base1 = (i + 1) * 3;
            if (base1 + 2 < ch.quatValues.size()) {
                float dt = ch.times[i + 1] - ch.times[i];
                Quat r = hermite(ch.quatValues[base0 + 1], ch.quatValues[base0 + 2],
                                 ch.quatValues[base1 + 1], ch.quatValues[base1 + 0], localT, dt);
                out.rotation = r.normalized();
                return;
            }
        }
        out.rotation = nlerp(keyAt(i), keyAt(i + 1), localT);
        return;
    }

    // Translation / Scale share the Vec3 path.
    if (ch.vec3Values.empty()) return;
    auto keyAt = [&](size_t k) -> Vec3 {
        size_t idx = cubic ? (k * 3 + 1) : k;
        return idx < ch.vec3Values.size() ? ch.vec3Values[idx] : Vec3();
    };

    Vec3 value;
    if (step) {
        value = keyAt(stepIndex(ch.times, time));
    } else if (n == 1) {
        value = keyAt(0);
    } else if (cubic) {
        size_t base0 = i * 3, base1 = (i + 1) * 3;
        if (base1 + 2 < ch.vec3Values.size()) {
            float dt = ch.times[i + 1] - ch.times[i];
            value = hermite(ch.vec3Values[base0 + 1], ch.vec3Values[base0 + 2],
                            ch.vec3Values[base1 + 1], ch.vec3Values[base1 + 0], localT, dt);
        } else {
            value = keyAt(i);
        }
    } else {
        value = lerp(keyAt(i), keyAt(i + 1), localT);
    }

    if (ch.path == AnimPath::Translation) out.position = value;
    else if (ch.path == AnimPath::Scale) out.scale = value;
    // AnimPath::Weights (morph targets) is parsed and stored but has no
    // renderer consumer yet -- see runtime/README.md's tier notes.
}

void sampleClip(const AnimClip& clip, float time, const std::vector<AnimNode>& nodes,
                std::vector<Transform>& out) {
    out.resize(nodes.size());
    // Start from the bind pose, not identity. A node with only a rotation
    // channel must keep its authored translation, or every unanimated bone
    // in a skeleton collapses to the origin -- the "character folds into a
    // single point" bug.
    for (size_t i = 0; i < nodes.size(); i++) out[i] = nodes[i].bindLocal;

    for (const auto& ch : clip.channels) {
        if (ch.targetNode < 0 || static_cast<size_t>(ch.targetNode) >= out.size()) continue;
        sampleChannel(ch, time, out[static_cast<size_t>(ch.targetNode)]);
    }
}

void composeHierarchy(const std::vector<AnimNode>& nodes, const std::vector<Transform>& local,
                      std::vector<Mat4>& outModel) {
    const size_t n = nodes.size();
    outModel.resize(n);
    for (size_t i = 0; i < n; i++) {
        Mat4 m = (i < local.size() ? local[i] : nodes[i].bindLocal).matrix();
        int p = nodes[i].parent;
        // Relies on the importer emitting parents before children (it walks
        // the glTF scene graph depth-first from the roots). The bounds check
        // on p < i makes a malformed file degrade to "treated as a root"
        // rather than reading an uninitialized matrix.
        if (p >= 0 && static_cast<size_t>(p) < i) {
            outModel[i] = outModel[static_cast<size_t>(p)] * m;
        } else {
            outModel[i] = m;
        }
    }
}

void blendPose(std::vector<Transform>& a, const std::vector<Transform>& b, float weight) {
    if (weight <= 0.0f) return;
    weight = clampf(weight, 0.0f, 1.0f);
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        a[i].position = lerp(a[i].position, b[i].position, weight);
        // slerp rather than nlerp here: a blend weight sweeps the whole
        // 0..1 range (unlike the tiny steps keyframe interpolation takes),
        // and nlerp's non-constant angular velocity is visible over that
        // distance as a limb that speeds up through the middle of a blend.
        a[i].rotation = slerp(a[i].rotation, b[i].rotation, weight);
        a[i].scale = lerp(a[i].scale, b[i].scale, weight);
    }
}

} // namespace pv
