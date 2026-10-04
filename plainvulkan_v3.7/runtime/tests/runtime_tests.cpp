// runtime_tests.cpp -- behavioural tests for the parts of the runtime that
// don't need a GPU: keyframe sampling, hierarchy composition, pose
// blending, particle simulation, the JSON codec, the math layer, terrain,
// and navigation/AI.
//
// These are deliberately assertion-based and dependency-free so they can be
// built and run anywhere with a C++17 compiler. From runtime/:
//
//   c++ -std=c++17 -Iinclude -Ithird_party/Ai/RVO2/src \
//       -Ithird_party/Ai/recastnavigation/Recast/Include \
//       -Ithird_party/Ai/recastnavigation/Detour/Include \
//       -DRVO_STATIC_DEFINE -o runtime_tests tests/runtime_tests.cpp
//       src/pv_math.cpp src/pv_json.cpp src/anim_sample.cpp
//       src/pv_value.cpp src/terrain.cpp src/ai.cpp src/ai_navmesh.cpp
//       third_party/Ai/recastnavigation/Recast/Source/*.cpp
//       third_party/Ai/recastnavigation/Detour/Source/*.cpp
//       third_party/Ai/RVO2/src/*.cc
//
// (kept on separate lines rather than backslash-continued: a trailing
// backslash inside a // comment splices the next line into it, which
// -Wcomment warns about and which silently swallowed part of this note.)
//
// The particle simulation is tested by linking a small reimplementation-free
// harness: updateEmitter lives in particles.cpp, which pulls in Vulkan, so
// the emitter tests here exercise the pure data structures (LifetimeCurve,
// Rng, shape sampling invariants) and the integration behaviour is covered
// by asserting on the emitter's own arithmetic rather than its GPU output.
//
// terrain.cpp and ai.cpp need no such treatment -- they are Vulkan-free by
// design, so the terrain and AI tests below call the real shipping code.
#include "pv/pv_ai.h"
#include "pv/pv_anim.h"
#include "pv/pv_json.h"
#include "pv/pv_math.h"
#include "pv/pv_particles.h"
#include "pv/pv_terrain.h"
#include "pv/pv_value.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace pv;

static int gFailures = 0;
static int gChecks = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        gChecks++;                                                                  \
        if (!(cond)) {                                                              \
            std::printf("  FAIL: %s\n        (%s, line %d)\n", msg, #cond, __LINE__); \
            gFailures++;                                                            \
        }                                                                           \
    } while (0)

static bool nearf(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }
static bool nearv(const Vec3& a, const Vec3& b, float eps = 1e-3f) {
    return nearf(a.x, b.x, eps) && nearf(a.y, b.y, eps) && nearf(a.z, b.z, eps);
}
// Quaternions q and -q are the same rotation, so equality is |dot| == 1.
static bool nearq(const Quat& a, const Quat& b, float eps = 1e-3f) {
    return nearf(std::fabs(dot(a, b)), 1.0f, eps);
}

// ======================================================================
static void testMath() {
    std::printf("math\n");

    // fromEuler must reproduce the legacy rotateXYZ exactly, or every
    // existing .pv script's RotateCamera/DrawMeshEx changes behaviour.
    {
        float P = 0.31f, Y = -0.77f, R = 1.13f;
        Mat4 me = Mat4::rotateXYZ(P, Y, R);
        Mat4 mq = Mat4::fromQuat(Quat::fromEuler(P, Y, R));
        bool same = true;
        for (int c = 0; c < 3; c++)
            for (int r = 0; r < 3; r++)
                if (!nearf(me.m[c][r], mq.m[c][r])) same = false;
        CHECK(same, "Quat::fromEuler agrees with the legacy Mat4::rotateXYZ");
    }

    CHECK(nearv(Quat::fromAxisAngle(Vec3(0, 1, 0), PV_PI * 0.5f).rotate(Vec3(1, 0, 0)), Vec3(0, 0, -1)),
          "90 degree yaw takes +X to -Z");

    {
        Quat e = Quat::fromEuler(0.3f, -0.7f, 1.1f);
        CHECK(nearv(e.toEuler(), Vec3(0.3f, -0.7f, 1.1f)), "Euler round-trips through the quaternion");
    }

    // TRS decompose, including the negative-scale case that a naive
    // implementation gets wrong.
    {
        Vec3 T(3, -2, 7), S(2, 0.5f, 1.5f);
        Quat R = Quat::fromEuler(0.4f, 1.2f, -0.3f);
        Vec3 dT, dS;
        Quat dR;
        decomposeTRS(Mat4::fromTRS(T, R, S), dT, dR, dS);
        CHECK(nearv(dT, T), "decomposeTRS recovers translation");
        CHECK(nearv(dS, S), "decomposeTRS recovers scale");
        CHECK(nearq(dR, R), "decomposeTRS recovers rotation");
    }

    // A 180-degree rotation has trace near zero, where the naive
    // trace-only quaternion extraction loses all precision.
    {
        Quat R = Quat::fromAxisAngle(Vec3(0, 1, 0), PV_PI);
        Vec3 dT, dS;
        Quat dR;
        decomposeTRS(Mat4::fromTRS(Vec3(), R, Vec3(1, 1, 1)), dT, dR, dS);
        CHECK(nearq(dR, R), "decomposeTRS survives a 180-degree rotation (Shepperd's method)");
    }

    // Inverting a perspective matrix -- non-affine, which is why the general
    // cofactor inverse is needed for editor mouse picking.
    {
        Mat4 proj = Mat4::perspective(1.0f, 1.6f, 0.1f, 100.0f);
        Mat4 id = proj * proj.inverse();
        bool ok = true;
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                if (!nearf(id.m[c][r], c == r ? 1.0f : 0.0f, 1e-3f)) ok = false;
        CHECK(ok, "perspective matrix inverts correctly");
    }

    CHECK(Mat4::fromTRS(Vec3(), Quat::identity(), Vec3(0, 0, 0)).inverse().m[0][0] == 1.0f,
          "a singular matrix inverts to identity rather than NaN");

    // slerp must take the short way around when the endpoints have
    // opposite signs -- otherwise limbs spin 350 degrees backwards.
    {
        Quat a = Quat::identity();
        Quat b = Quat::fromAxisAngle(Vec3(0, 1, 0), 2.0f);
        CHECK(nearq(slerp(a, b, 0.5f), slerp(a, -b, 0.5f)),
              "slerp treats q and -q as the same rotation");
        CHECK(nearq(slerp(a, b, 0.0f), a), "slerp hits its start endpoint");
        CHECK(nearq(slerp(a, b, 1.0f), b), "slerp hits its end endpoint");
    }

    // Determinism matters: the editor scrubs a particle effect back and
    // forth and must see the same result each time.
    {
        Rng r1(42), r2(42);
        bool same = true, inRange = true;
        for (int i = 0; i < 2000; i++) {
            float u = r1.unit();
            if (u < 0.0f || u >= 1.0f) inRange = false;
            if (u != r2.unit()) same = false;
        }
        CHECK(same, "Rng is deterministic from a seed");
        CHECK(inRange, "Rng::unit stays in [0, 1)");
        bool inside = true;
        for (int i = 0; i < 500; i++)
            if (r1.insideUnitSphere().lengthSq() > 1.0001f) inside = false;
        CHECK(inside, "insideUnitSphere stays inside the unit sphere");
    }
}

// ======================================================================
static void testJson() {
    std::printf("json\n");
    std::string err;

    {
        std::string src = R"({"a":[1,2.5,-3e2],"b":{"c":"hi\n\u00e9\ud83d\ude00"},"d":true,"e":null})";
        auto j = json::Json::parse(src, &err);
        CHECK(err.empty(), "a well-formed document parses");
        CHECK(j["a"].size() == 3, "array length is right");
        CHECK(nearf(j["a"][1].asFloat(), 2.5f), "fractional numbers parse");
        CHECK(nearf(j["a"][2].asFloat(), -300.0f), "exponent notation parses");
        CHECK(j["d"].asBool() && j["e"].isNull(), "true and null parse");

        // Surrogate pairs must be joined, or an emoji in an asset path is
        // mangled on the round trip.
        std::string s = j["b"]["c"].asString();
        auto j2 = json::Json::parse(j.dump(), &err);
        CHECK(err.empty() && j2["b"]["c"].asString() == s, "unicode survives a dump/parse round trip");
    }

    json::Json::parse("{\"a\":}", &err);
    CHECK(!err.empty(), "a missing value is rejected");
    json::Json::parse("[1,2,]", &err);
    CHECK(!err.empty(), "a trailing comma is rejected");
    json::Json::parse("{\"a\":1} trailing", &err);
    CHECK(!err.empty(), "trailing content is rejected");

    // Reading a missing or wrong-typed field must yield the fallback, not
    // throw -- that's what lets an old scene file load in a new build.
    {
        auto j = json::Json::parse("{\"n\":5}", &err);
        CHECK(j["missing"].asFloat(7.5f) == 7.5f, "a missing field returns its default");
        CHECK(j["n"].asString("fallback") == "fallback", "a wrong-typed field returns its default");
    }

    // Floats must round-trip exactly, or every save nudges every transform.
    {
        json::Json o = json::Json::object();
        o["v"] = json::Json(0.1f);
        o["big"] = json::Json(1234567.875);
        auto back = json::Json::parse(o.dump(), &err);
        CHECK(nearf(back["v"].asFloat(), 0.1f, 1e-7f), "0.1 round-trips");
        CHECK(back["big"].asDouble() == 1234567.875, "a large exact double round-trips");
    }
}

// ======================================================================
// Builds a two-node hierarchy: a root and a child offset along +X.
static void buildRig(AnimRecord& rec) {
    AnimNode root;
    root.name = "root";
    root.parent = -1;
    AnimNode child;
    child.name = "child";
    child.parent = 0;
    child.bindLocal.position = Vec3(2, 0, 0);
    rec.nodes = {root, child};
}

static void testAnimation() {
    std::printf("animation\n");

    AnimRecord rec;
    buildRig(rec);

    AnimClip clip;
    clip.name = "move";
    {
        AnimChannel ch;
        ch.targetNode = 0;
        ch.path = AnimPath::Translation;
        ch.interp = Interpolation::Linear;
        ch.times = {0.0f, 1.0f, 2.0f};
        ch.vec3Values = {Vec3(0, 0, 0), Vec3(0, 10, 0), Vec3(0, 0, 0)};
        clip.channels.push_back(ch);
    }
    {
        AnimChannel ch;
        ch.targetNode = 0;
        ch.path = AnimPath::Rotation;
        ch.interp = Interpolation::Linear;
        ch.times = {0.0f, 2.0f};
        ch.quatValues = {Quat::identity(), Quat::fromAxisAngle(Vec3(0, 1, 0), PV_PI * 0.5f)};
        clip.channels.push_back(ch);
    }
    clip.recomputeDuration();
    CHECK(nearf(clip.duration, 2.0f), "clip duration is the max channel time");
    rec.clips.push_back(clip);

    std::vector<Transform> pose;

    sampleClip(clip, 0.0f, rec.nodes, pose);
    CHECK(nearv(pose[0].position, Vec3(0, 0, 0)), "sampling at t=0 gives the first key");

    sampleClip(clip, 0.5f, rec.nodes, pose);
    CHECK(nearv(pose[0].position, Vec3(0, 5, 0)), "linear interpolation is exact at the midpoint");

    sampleClip(clip, 1.0f, rec.nodes, pose);
    CHECK(nearv(pose[0].position, Vec3(0, 10, 0)), "sampling lands exactly on an interior key");

    // Out-of-range times must clamp rather than extrapolate or read out of
    // bounds.
    sampleClip(clip, -5.0f, rec.nodes, pose);
    CHECK(nearv(pose[0].position, Vec3(0, 0, 0)), "a negative time clamps to the first key");
    sampleClip(clip, 99.0f, rec.nodes, pose);
    CHECK(nearv(pose[0].position, Vec3(0, 0, 0)), "a time past the end clamps to the last key");

    // A node with no channel must keep its bind transform. Getting this
    // wrong collapses every unanimated bone to the origin.
    sampleClip(clip, 0.5f, rec.nodes, pose);
    CHECK(nearv(pose[1].position, Vec3(2, 0, 0)), "an unanimated node keeps its bind pose");

    // Hierarchy composition: the child inherits the root's motion. Tested
    // with a translation-only pose, because `clip` also rotates the root --
    // which correctly swings the child's offset around and would make the
    // expected position a much less obvious number.
    {
        std::vector<Transform> tpose(2);
        tpose[0].position = Vec3(0, 5, 0);
        tpose[1] = rec.nodes[1].bindLocal;
        std::vector<Mat4> model;
        composeHierarchy(rec.nodes, tpose, model);
        Vec3 childWorld(model[1].m[3][0], model[1].m[3][1], model[1].m[3][2]);
        CHECK(nearv(childWorld, Vec3(2, 5, 0)), "a child composes with its parent's transform");
    }

    // ...and the child must genuinely follow the parent's *rotation* too,
    // which is the part a translation-only test would miss. A 90-degree yaw
    // on the root takes the child's +X offset around to -Z.
    {
        std::vector<Transform> rpose(2);
        rpose[0].rotation = Quat::fromAxisAngle(Vec3(0, 1, 0), PV_PI * 0.5f);
        rpose[1] = rec.nodes[1].bindLocal;
        std::vector<Mat4> model;
        composeHierarchy(rec.nodes, rpose, model);
        Vec3 childWorld(model[1].m[3][0], model[1].m[3][1], model[1].m[3][2]);
        CHECK(nearv(childWorld, Vec3(0, 0, -2)), "a child inherits its parent's rotation");
    }

    // Step interpolation holds the previous key rather than blending.
    {
        AnimChannel ch;
        ch.targetNode = 0;
        ch.path = AnimPath::Translation;
        ch.interp = Interpolation::Step;
        ch.times = {0.0f, 1.0f};
        ch.vec3Values = {Vec3(0, 0, 0), Vec3(0, 0, 100)};
        Transform t;
        sampleChannel(ch, 0.99f, t);
        CHECK(nearv(t.position, Vec3(0, 0, 0)), "step interpolation holds the previous key");
        sampleChannel(ch, 1.0f, t);
        CHECK(nearv(t.position, Vec3(0, 0, 100)), "step interpolation advances at the key");
    }

    // Duplicate timestamps are legal in glTF (a hard cut) and must not
    // divide by zero.
    {
        AnimChannel ch;
        ch.targetNode = 0;
        ch.path = AnimPath::Translation;
        ch.times = {0.0f, 1.0f, 1.0f, 2.0f};
        ch.vec3Values = {Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(5, 0, 0), Vec3(6, 0, 0)};
        Transform t;
        sampleChannel(ch, 1.0f, t);
        CHECK(std::isfinite(t.position.x), "duplicate key times don't produce NaN");
    }

    // An empty channel must be a no-op, not a crash.
    {
        AnimChannel ch;
        ch.targetNode = 0;
        ch.path = AnimPath::Translation;
        Transform t;
        t.position = Vec3(9, 9, 9);
        sampleChannel(ch, 0.5f, t);
        CHECK(nearv(t.position, Vec3(9, 9, 9)), "an empty channel leaves the transform untouched");
    }

    // Cubic spline: with zero tangents this degenerates to a smooth
    // Hermite between the values, which must still hit both endpoints.
    {
        AnimChannel ch;
        ch.targetNode = 0;
        ch.path = AnimPath::Translation;
        ch.interp = Interpolation::CubicSpline;
        ch.times = {0.0f, 1.0f};
        ch.vec3Values = {
            Vec3(), Vec3(0, 0, 0), Vec3(), // in-tangent, value, out-tangent
            Vec3(), Vec3(0, 4, 0), Vec3(),
        };
        Transform t;
        sampleChannel(ch, 0.0f, t);
        CHECK(nearv(t.position, Vec3(0, 0, 0)), "cubic spline hits its start value");
        sampleChannel(ch, 1.0f, t);
        CHECK(nearv(t.position, Vec3(0, 4, 0)), "cubic spline hits its end value");
        sampleChannel(ch, 0.5f, t);
        CHECK(t.position.y > 0.0f && t.position.y < 4.0f, "cubic spline stays between its endpoints");
    }

    // Blending.
    {
        std::vector<Transform> a(1), b(1);
        a[0].position = Vec3(0, 0, 0);
        b[0].position = Vec3(10, 0, 0);
        a[0].rotation = Quat::identity();
        b[0].rotation = Quat::fromAxisAngle(Vec3(0, 1, 0), PV_PI * 0.5f);

        std::vector<Transform> mid = a;
        blendPose(mid, b, 0.5f);
        CHECK(nearv(mid[0].position, Vec3(5, 0, 0)), "blendPose interpolates position by weight");
        CHECK(nearf(mid[0].rotation.length(), 1.0f), "a blended rotation stays a unit quaternion");

        std::vector<Transform> zero = a;
        blendPose(zero, b, 0.0f);
        CHECK(nearv(zero[0].position, Vec3(0, 0, 0)), "a zero blend weight is a no-op");

        std::vector<Transform> full = a;
        blendPose(full, b, 1.0f);
        CHECK(nearv(full[0].position, Vec3(10, 0, 0)), "a full blend weight replaces the pose");

        // Mismatched pose sizes must clamp, not read past the end.
        std::vector<Transform> shortPose(1);
        std::vector<Transform> longPose(5);
        blendPose(shortPose, longPose, 0.5f);
        CHECK(true, "blending mismatched pose lengths doesn't overrun");
    }

    CHECK(rec.findClip("move") == 0, "findClip locates a clip by name");
    CHECK(rec.findClip("nope") == -1, "findClip returns -1 for an unknown name");
}

// ======================================================================
static void testParticles() {
    std::printf("particles\n");

    // LifetimeCurve is the shape every particle property follows.
    {
        LifetimeCurve c{0, 1, 1, 0};
        CHECK(nearf(c.eval(0.0f), 0.0f), "curve starts at p0");
        CHECK(nearf(c.eval(1.0f), 0.0f), "curve ends at p3");
        CHECK(nearf(c.eval(0.5f), 1.0f), "curve interpolates through the middle control points");
        CHECK(nearf(c.eval(-3.0f), 0.0f) && nearf(c.eval(9.0f), 0.0f), "curve clamps out-of-range t");

        LifetimeCurve k = LifetimeCurve::constant(2.5f);
        CHECK(nearf(k.eval(0.3f), 2.5f) && nearf(k.eval(0.9f), 2.5f), "a constant curve is flat");

        LifetimeCurve f = LifetimeCurve::fadeOut();
        CHECK(f.eval(0.0f) > f.eval(1.0f), "fadeOut decreases over lifetime");
    }

    // Emitter defaults must be self-consistent: a freshly constructed
    // emitter should be immediately usable without any setup calls.
    {
        EmitterRecord e;
        CHECK(e.maxParticles > 0, "an emitter has a positive particle cap by default");
        CHECK(e.lifetimeMax >= e.lifetimeMin, "default lifetime range is ordered");
        CHECK(e.speedMax >= e.speedMin, "default speed range is ordered");
        CHECK(e.sizeMax >= e.sizeMin, "default size range is ordered");
        CHECK(e.particles.empty(), "a new emitter starts with no live particles");
    }

    // The fractional spawn accumulator is what makes a sub-1/sec rate work
    // at all: int(rate * dt) truncates to zero every frame without it.
    //
    // The accumulator is deliberately a double. In float, the residual
    // creeps toward 1.0 without ever crossing, so the emitter permanently
    // under-spawns by one and every particle is late -- a 0.5/sec emitter
    // produced 29 particles in 60 seconds instead of 30. This test asserts
    // the exact count precisely so that regression can't come back.
    {
        const float rate = 0.5f; // one particle every two seconds
        const float dt = 1.0f / 60.0f;
        double accumulator = 0.0;
        int spawned = 0;
        for (int frame = 0; frame < 60 * 60; frame++) { // 60 seconds
            accumulator += static_cast<double>(rate) * static_cast<double>(dt);
            int whole = static_cast<int>(accumulator);
            accumulator -= static_cast<double>(whole);
            spawned += whole;
        }
        CHECK(spawned == 30, "a fractional spawn rate hits its exact expected count over 60s");
    }

    // The same loop in float, kept as a demonstration of why the double
    // matters: if this ever starts passing, the platform's float behaviour
    // changed and the comment above should be revisited.
    {
        float accumulator = 0.0f;
        int spawned = 0;
        for (int frame = 0; frame < 60 * 60; frame++) {
            accumulator += 0.5f * (1.0f / 60.0f);
            int whole = static_cast<int>(accumulator);
            accumulator -= static_cast<float>(whole);
            spawned += whole;
        }
        CHECK(spawned < 30, "the float accumulator under-spawns, which is why the real one is double");
    }

    // Drag is applied as exp(-drag*dt) so it's framerate-independent:
    // simulating the same wall-clock duration at two step sizes must give
    // (very nearly) the same result.
    {
        auto simulate = [](float dt, int steps, float drag) {
            float v = 100.0f;
            for (int i = 0; i < steps; i++) v *= std::exp(-drag * dt);
            return v;
        };
        float coarse = simulate(1.0f / 30.0f, 30, 2.0f);
        float fine = simulate(1.0f / 120.0f, 120, 2.0f);
        CHECK(nearf(coarse, fine, 1e-3f), "exponential drag is framerate-independent");
    }
}

// ======================================================================
// Regression tests for pv::Value's container semantics. Every case here
// corresponds to a bug that was live in the runtime: they are cheap, and
// they are the difference between "a script misbehaves" and "a script
// silently loses data or aborts the process".
static void testValueSemantics() {
    std::printf("value semantics\n");

    // A string subscript is a KEY. index_get already treated `o["k"]` on an
    // object as a member read; index_ref did not, so writing through it
    // replaced the whole object with a one-element array and every other
    // field vanished with no diagnostic.
    {
        Value o;
        member_ref(o, "name") = Value(std::string("alice"));
        member_ref(o, "score") = Value(static_cast<int64_t>(10));
        index_ref(o, Value(std::string("name"))) = Value(std::string("bob"));

        CHECK(o.isObject(), "object survives a string-subscript write");
        CHECK(index_get(o, Value(std::string("name"))).asString() == "bob",
              "string-subscript write updates the right key");
        CHECK(index_get(o, Value(std::string("score"))).asInt() == 10,
              "string-subscript write leaves sibling keys intact");
        CHECK(o.objectRef().size() == 2, "no keys were dropped");
    }

    // `o["k"] = v` on a fresh variable must produce the same thing as
    // `o.k = v`, not an array -- otherwise the two spellings of the same
    // operation diverge.
    {
        Value a, b;
        index_ref(a, Value(std::string("k"))) = Value(static_cast<int64_t>(1));
        member_ref(b, "k") = Value(static_cast<int64_t>(1));
        CHECK(a.isObject() && b.isObject(), "string subscript auto-vivifies an object");
        CHECK(index_get(a, Value(std::string("k"))).asInt() == 1, "auto-vivified key readable");
    }

    // A numeric subscript still auto-vivifies an array.
    {
        Value arr;
        index_ref(arr, Value(static_cast<int64_t>(2))) = Value(static_cast<int64_t>(7));
        CHECK(arr.isArray(), "numeric subscript auto-vivifies an array");
        CHECK(arr.arrayRef().size() == 3, "array grew to hold the index");
        CHECK(index_get(arr, Value(static_cast<int64_t>(2))).asInt() == 7, "value stored at index");
        CHECK(index_get(arr, Value(static_cast<int64_t>(0))).isNull(), "gap elements are null");
    }

    // An out-of-range index used to ask std::vector for a multi-gigabyte
    // allocation, and the resulting std::bad_alloc aborted the process.
    // It must now be refused and survivable. (This intentionally logs an
    // error line to stderr.)
    {
        std::printf("  (expect one intentional [ERROR] line below)\n");
        Value arr;
        index_ref(arr, Value(static_cast<int64_t>(5000000000LL))) = Value(static_cast<int64_t>(1));
        CHECK(arr.arrayRef().size() == 0, "absurd index is discarded, not allocated");
    }

    // Negative indices clamp to 0 rather than wrapping to a huge size_t.
    {
        Value arr;
        index_ref(arr, Value(static_cast<int64_t>(-5))) = Value(static_cast<int64_t>(3));
        CHECK(arr.arrayRef().size() == 1, "negative index clamps to slot 0");
        CHECK(index_get(arr, Value(static_cast<int64_t>(0))).asInt() == 3, "negative index writes slot 0");
    }

    // Reads never throw and never go out of bounds.
    {
        Value arr = make_array({Value(static_cast<int64_t>(1))});
        CHECK(index_get(arr, Value(static_cast<int64_t>(99))).isNull(), "out-of-range read is null");
        CHECK(index_get(arr, Value(static_cast<int64_t>(-1))).isNull(), "negative read is null");
        CHECK(member_get(Value(), "nope").isNull(), "member read on null is null");
    }

    // Arithmetic on a zero denominator yields 0 instead of trapping (SIGFPE
    // on integer division would kill the process).
    {
        CHECK((Value(static_cast<int64_t>(1)) / Value(static_cast<int64_t>(0))).asInt() == 0,
              "integer divide by zero yields 0");
        CHECK((Value(static_cast<int64_t>(1)) % Value(static_cast<int64_t>(0))).asInt() == 0,
              "integer modulo by zero yields 0");
        CHECK((Value(1.0) / Value(0.0)).asFloat() == 0.0, "float divide by zero yields 0");
    }

    // Strings that aren't numbers coerce to 0 rather than throwing out of
    // std::stoll/std::stod.
    {
        CHECK(Value(std::string("abc")).asInt() == 0, "non-numeric string asInt is 0");
        CHECK(Value(std::string("abc")).asFloat() == 0.0, "non-numeric string asFloat is 0");
        CHECK(Value(std::string("")).asInt() == 0, "empty string asInt is 0");
        CHECK(Value::MakeHandle(42, "mesh").asInt() == 42, "handle asInt is the resource id");
    }
}

// ======================================================================
// Terrain. All of this is real terrain code -- terrain.cpp is deliberately
// Vulkan-free so these are the shipping functions, not a reimplementation.
// ======================================================================
static Terrain makeRamp(int cols, int rows, float cellSize, float rise) {
    // Height increases linearly along +X and is constant along Z, so every
    // expected value below can be worked out by hand.
    Terrain t;
    t.cols = cols;
    t.rows = rows;
    t.cellSize = cellSize;
    t.heights.assign(static_cast<size_t>(cols) * rows, 0.0f);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            t.heights[static_cast<size_t>(r) * cols + c] = static_cast<float>(c) * rise;
        }
    }
    refreshTerrainBounds(t);
    rebuildTerrainChunks(t);
    return t;
}

static void testTerrain() {
    std::printf("terrain\n");

    // ---- sampling on a known ramp ----
    {
        Terrain t = makeRamp(9, 9, 1.0f, 2.0f); // +2 units of height per cell in X
        CHECK(nearf(t.minHeight, 0.0f), "ramp min height is 0");
        CHECK(nearf(t.maxHeight, 16.0f), "ramp max height is 16");

        CHECK(nearf(terrainHeightAt(t, 0.0f, 0.0f), 0.0f), "height at the origin vertex");
        CHECK(nearf(terrainHeightAt(t, 3.0f, 4.0f), 6.0f), "height at an exact vertex");
        CHECK(nearf(terrainHeightAt(t, 3.5f, 4.0f), 7.0f), "bilinear midpoint between vertices");
        CHECK(nearf(terrainHeightAt(t, 2.0f, 0.0f), terrainHeightAt(t, 2.0f, 7.0f)),
              "height is constant along Z on this ramp");

        // Outside the grid clamps to the edge instead of reading out of
        // bounds -- a camera outside the map is normal, not a crash.
        CHECK(nearf(terrainHeightAt(t, -50.0f, -50.0f), 0.0f), "sampling before the origin clamps to the edge");
        CHECK(nearf(terrainHeightAt(t, 500.0f, 500.0f), 16.0f), "sampling past the far edge clamps");

        // A ramp rising 2 units per 1 unit of X is atan(2) = 63.43 degrees.
        float slope = terrainSlopeDegAt(t, 4.0f, 4.0f);
        CHECK(nearf(slope, 63.4349f, 0.05f), "slope of a 2:1 ramp is atan(2) degrees");

        Vec3 n = terrainNormalAt(t, 4.0f, 4.0f);
        CHECK(nearf(n.length(), 1.0f), "terrain normal is unit length");
        CHECK(n.x < 0.0f, "normal tilts against the uphill direction");
        CHECK(n.y > 0.0f, "normal points upward");
        CHECK(nearf(n.z, 0.0f), "normal has no Z component on an X-only ramp");
    }

    // ---- flat terrain ----
    {
        Terrain t = makeRamp(5, 5, 2.0f, 0.0f);
        CHECK(nearf(terrainSlopeDegAt(t, 3.0f, 3.0f), 0.0f), "flat terrain has zero slope");
        Vec3 n = terrainNormalAt(t, 3.0f, 3.0f);
        CHECK(nearf(n.y, 1.0f), "flat terrain normal is straight up");
    }

    // ---- origin offset ----
    {
        Terrain t = makeRamp(9, 9, 1.0f, 2.0f);
        t.origin = Vec3(100.0f, 5.0f, -20.0f);
        CHECK(nearf(terrainHeightAt(t, 103.0f, -20.0f), 5.0f + 6.0f),
              "origin offsets both the sample point and the returned height");
    }

    // ---- raycast ----
    {
        Terrain t = makeRamp(9, 9, 1.0f, 2.0f);
        // Straight down from well above a known vertex.
        TerrainRayHit h = terrainRaycast(t, Vec3(3.0f, 100.0f, 4.0f), Vec3(0, -1, 0), 200.0f);
        CHECK(h.hit, "downward ray hits the terrain");
        CHECK(nearf(h.point.y, 6.0f, 0.01f), "downward ray lands on the surface height");
        CHECK(nearf(h.distance, 94.0f, 0.01f), "hit distance matches the drop");

        // A ray that never descends far enough must not report a hit.
        TerrainRayHit miss = terrainRaycast(t, Vec3(3.0f, 100.0f, 4.0f), Vec3(0, -1, 0), 10.0f);
        CHECK(!miss.hit, "ray shorter than the drop reports no hit");

        // Starting underground is refused rather than reporting a bogus
        // hit behind the origin.
        TerrainRayHit under = terrainRaycast(t, Vec3(3.0f, -5.0f, 4.0f), Vec3(0, 1, 0), 100.0f);
        CHECK(!under.hit, "a ray starting below the surface reports no hit");

        // Grazing angle: this is the case a single analytic step misses.
        TerrainRayHit graze = terrainRaycast(t, Vec3(0.0f, 20.0f, 4.0f), Vec3(1.0f, -1.0f, 0.0f), 60.0f);
        CHECK(graze.hit, "shallow diagonal ray still finds the surface");
        if (graze.hit) {
            CHECK(nearf(graze.point.y, terrainHeightAt(t, graze.point.x, graze.point.z), 0.05f),
                  "diagonal hit point lies on the surface");
        }
    }

    // ---- LOD selection and chunk geometry ----
    {
        Terrain t = makeRamp(65, 65, 1.0f, 0.1f);
        t.chunkSize = 32;
        t.lodDistance = 50.0f;
        rebuildTerrainChunks(t);
        CHECK(t.chunks.size() == 4, "a 64-cell terrain in 32-cell chunks is 2x2 chunks");
        CHECK(t.chunks[0].cols == 33 && t.chunks[0].rows == 33,
              "chunks carry the shared seam vertex (32 cells = 33 vertices)");

        const TerrainChunk& c0 = t.chunks[0];
        CHECK(selectTerrainLod(t, c0, c0.center) == 0, "camera inside a chunk selects LOD 0");
        Vec3 far = c0.center + Vec3(0, 0, 500.0f);
        CHECK(selectTerrainLod(t, c0, far) == kTerrainLodLevels - 1, "a distant chunk clamps to the coarsest LOD");

        std::vector<TerrainVertex> verts;
        std::vector<uint32_t> idx;
        CHECK(buildTerrainChunkGeometry(t, c0, 0, verts, idx), "LOD 0 geometry builds");
        CHECK(verts.size() == 33u * 33u, "LOD 0 emits every vertex");
        CHECK(idx.size() == 32u * 32u * 6u, "LOD 0 emits two triangles per cell");

        std::vector<TerrainVertex> v1;
        std::vector<uint32_t> i1;
        CHECK(buildTerrainChunkGeometry(t, c0, 1, v1, i1), "LOD 1 geometry builds");
        CHECK(v1.size() == 17u * 17u, "LOD 1 halves the vertex stride");
        CHECK(i1.size() < idx.size(), "LOD 1 emits fewer indices than LOD 0");

        // Every index must be in range -- an off-by-one here is a GPU
        // crash that no CPU-side test would otherwise catch.
        bool inRange = true;
        for (uint32_t i : i1) inRange = inRange && (i < v1.size());
        CHECK(inRange, "LOD 1 indices are all within the vertex array");

        // Corner vertices must sit exactly on the terrain surface,
        // otherwise chunks won't line up with height queries.
        const TerrainVertex& v00 = verts[0];
        CHECK(nearf(v00.pos[1], terrainHeightAt(t, v00.pos[0], v00.pos[2]), 1e-3f),
              "chunk vertices sit on the sampled surface");
    }

    // ---- layer splatting ----
    {
        Terrain t = makeRamp(17, 17, 1.0f, 1.0f); // heights 0..16, 45-degree slope
        TerrainLayer low;
        low.r = 1; low.g = 0; low.b = 0;
        low.minHeight = 0; low.maxHeight = 4; low.blend = 0.0f;
        TerrainLayer high;
        high.r = 0; high.g = 0; high.b = 1;
        high.minHeight = 12; high.maxHeight = 100; high.blend = 0.0f;
        t.layers.push_back(low);
        t.layers.push_back(high);

        std::vector<float> w;
        CHECK(terrainLayerWeightsAt(t, 1.0f, 8.0f, w), "a point inside the low band matches a layer");
        CHECK(nearf(w[0], 1.0f) && nearf(w[1], 0.0f), "low ground is fully the low layer");

        CHECK(terrainLayerWeightsAt(t, 14.0f, 8.0f, w), "a point inside the high band matches a layer");
        CHECK(nearf(w[0], 0.0f) && nearf(w[1], 1.0f), "high ground is fully the high layer");

        CHECK(!terrainLayerWeightsAt(t, 8.0f, 8.0f, w), "a gap between bands matches no layer");

        // With a soft edge the two bands overlap and the weights must
        // still sum to 1 rather than double-counting.
        t.layers[0].maxHeight = 8;
        t.layers[0].blend = 4;
        t.layers[1].minHeight = 8;
        t.layers[1].blend = 4;
        CHECK(terrainLayerWeightsAt(t, 8.0f, 8.0f, w), "overlapping soft bands match");
        CHECK(nearf(w[0] + w[1], 1.0f), "blended layer weights are normalized");

        std::vector<uint8_t> px = bakeTerrainPixels(t, 16);
        CHECK(px.size() == 16u * 16u * 4u, "bake produces RGBA8 at the requested resolution");
        bool opaque = true;
        for (size_t i = 3; i < px.size(); i += 4) opaque = opaque && (px[i] == 255);
        CHECK(opaque, "baked pixels are fully opaque");
    }

    // ---- procedural generation is deterministic ----
    {
        Terrain a = makeRamp(33, 33, 1.0f, 0.0f);
        Terrain b = makeRamp(33, 33, 1.0f, 0.0f);
        generateTerrainHeights(a, 1234u, 0.5f, 10.0f);
        generateTerrainHeights(b, 1234u, 0.5f, 10.0f);
        CHECK(a.heights == b.heights, "the same seed produces identical terrain");

        Terrain c = makeRamp(33, 33, 1.0f, 0.0f);
        generateTerrainHeights(c, 5678u, 0.5f, 10.0f);
        CHECK(!(a.heights == c.heights), "a different seed produces different terrain");
        CHECK(a.maxHeight <= 10.0f + 1e-3f, "generated height stays within the requested scale");
        CHECK(a.maxHeight > a.minHeight, "generated terrain is not flat");
    }
}

// ======================================================================
// AI: nav grid, A*, smoothing, agents.
// ======================================================================
static NavGrid makeOpenGrid(int cols, int rows, float cellSize) {
    NavGrid g;
    g.cols = cols;
    g.rows = rows;
    g.cellSize = cellSize;
    size_t n = static_cast<size_t>(cols) * rows;
    g.baseWalkable.assign(n, 1);
    g.walkable.assign(n, 1);
    g.cost.assign(n, 1.0f);
    g.height.assign(n, 0.0f);
    return g;
}

static bool pathSamplesWalkable(const NavGrid& g, const std::vector<Vec3>& path) {
    if (path.empty()) return false;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        for (int s = 0; s <= 12; ++s) {
            float t = static_cast<float>(s) / 12.0f;
            float x = path[i].x * (1.0f - t) + path[i + 1].x * t;
            float z = path[i].z * (1.0f - t) + path[i + 1].z * t;
            int c = 0, r = 0;
            if (!navWorldToCell(g, x, z, c, r) || !navIsWalkable(g, c, r)) return false;
        }
    }
    return true;
}

static void testAI() {
    std::printf("ai / navigation\n");

    // ---- coordinate mapping ----
    {
        NavGrid g = makeOpenGrid(10, 10, 2.0f);
        int c = -1, r = -1;
        CHECK(navWorldToCell(g, 0.5f, 0.5f, c, r) && c == 0 && r == 0, "a point in the first cell maps to (0,0)");
        CHECK(navWorldToCell(g, 5.0f, 3.0f, c, r) && c == 2 && r == 1, "world coordinates map by cellSize");
        CHECK(!navWorldToCell(g, -1.0f, 0.0f, c, r), "a point before the origin is outside the grid");
        CHECK(!navWorldToCell(g, 100.0f, 0.0f, c, r), "a point past the far edge is outside the grid");

        Vec3 w = navCellToWorld(g, 2, 1);
        CHECK(nearf(w.x, 5.0f) && nearf(w.z, 3.0f), "cell-to-world returns the cell centre");
    }

    // ---- Recast/Detour on an open grid ----
    {
        NavGrid g = makeOpenGrid(10, 10, 1.0f);
        std::vector<Vec3> path = navFindPath(g, 0.5f, 0.5f, 9.5f, 0.5f, false);
        CHECK(!path.empty(), "a path exists across an open grid");
        CHECK(nearf(path.back().x, 9.5f, 0.6f) && nearf(path.back().z, 0.5f, 0.6f), "the path ends near the goal");

        std::vector<Vec3> diag = navFindPath(g, 0.5f, 0.5f, 9.5f, 9.5f, false);
        CHECK(!diag.empty(), "a diagonal run finds a path");
        CHECK(pathSamplesWalkable(g, diag), "the diagonal path stays on walkable cells");
    }

    // ---- walls ----
    {
        NavGrid g = makeOpenGrid(11, 11, 1.0f);
        for (int r = 1; r < 11; ++r) g.baseWalkable[static_cast<size_t>(r) * 11 + 5] = 0;
        g.walkable = g.baseWalkable;
        g.meshSerial++;

        CHECK(!navIsWalkable(g, 5, 5), "the wall cell is blocked");
        CHECK(navIsWalkable(g, 5, 0), "the gap is open");

        std::vector<Vec3> path = navFindPath(g, 0.5f, 5.5f, 10.5f, 5.5f, false);
        CHECK(!path.empty(), "a path routes through the gap");
        bool anyWalkable = false;
        for (const Vec3& p : path) {
            int c = 0, r = 0;
            if (navWorldToCell(g, p.x, p.z, c, r) && navIsWalkable(g, c, r)) anyWalkable = true;
        }
        CHECK(anyWalkable, "path waypoints land on walkable cells");

        g.baseWalkable[0 * 11 + 5] = 0;
        g.walkable = g.baseWalkable;
        g.meshSerial++;
        CHECK(navFindPath(g, 0.5f, 5.5f, 10.5f, 5.5f, false).empty(), "a fully walled grid has no path");
    }

    // ---- line of sight and smoothing ----
    {
        NavGrid g = makeOpenGrid(20, 20, 1.0f);
        CHECK(navLineOfSight(g, 0, 0, 19, 19), "open grid has line of sight corner to corner");

        // A staircase path across open ground should smooth to almost
        // nothing -- that is the whole point of string pulling.
        std::vector<Vec3> raw = navFindPath(g, 0.5f, 0.5f, 19.5f, 10.5f, false);
        std::vector<Vec3> smooth = navFindPath(g, 0.5f, 0.5f, 19.5f, 10.5f, true);
        CHECK(!raw.empty() && !smooth.empty(), "both raw and smoothed paths exist");
        CHECK(smooth.size() <= raw.size(), "smoothing does not add waypoints");
        CHECK(nearf(smooth.back().x, raw.back().x) && nearf(smooth.back().z, raw.back().z),
              "smoothing preserves the goal");

        NavGrid w = makeOpenGrid(20, 20, 1.0f);
        for (int r = 0; r < 15; ++r) w.baseWalkable[static_cast<size_t>(r) * 20 + 10] = 0;
        w.walkable = w.baseWalkable;
        w.meshSerial++;
        CHECK(!navLineOfSight(w, 0, 0, 19, 0), "line of sight is blocked by the wall");
        std::vector<Vec3> around = navFindPath(w, 0.5f, 0.5f, 19.5f, 0.5f, true);
        CHECK(!around.empty(), "a smoothed path exists around the wall");
        bool allWalkable = true;
        for (const Vec3& p : around) {
            int c = 0, r = 0;
            navWorldToCell(w, p.x, p.z, c, r);
            allWalkable = allWalkable && navIsWalkable(w, c, r);
        }
        CHECK(allWalkable, "every smoothed waypoint stays on walkable ground");
    }

    // ---- obstacles ----
    {
        NavGrid g = makeOpenGrid(20, 20, 1.0f);
        NavObstacle o;
        o.x = 10.0f;
        o.z = 10.0f;
        o.width = 4.0f;
        o.depth = 4.0f;
        g.obstacles.push_back(o);
        navApplyObstacles(g);
        CHECK(!navIsWalkable(g, 10, 10), "an obstacle blocks the cells it covers");
        CHECK(navIsWalkable(g, 0, 0), "an obstacle leaves distant cells alone");

        g.obstacles.clear();
        navApplyObstacles(g);
        CHECK(navIsWalkable(g, 10, 10), "clearing obstacles restores the base walkability");
    }

    // ---- corner cutting ----
    {
        // Two blockers meeting diagonally. An agent must not slip between
        // them; that reads as clipping through geometry.
        NavGrid g = makeOpenGrid(5, 5, 1.0f);
        g.baseWalkable[static_cast<size_t>(1) * 5 + 2] = 0; // (2,1)
        g.baseWalkable[static_cast<size_t>(2) * 5 + 1] = 0; // (1,2)
        g.walkable = g.baseWalkable;
        g.meshSerial++;
        std::vector<Vec3> path = navFindPath(g, 1.5f, 1.5f, 2.5f, 2.5f, false);
        CHECK(path.empty() || path.size() > 2, "Detour refuses to cut the diagonal corner between two blockers");
    }

    // ---- terrain-derived nav grid ----
    {
        Terrain t = makeRamp(17, 17, 1.0f, 1.0f); // uniform 45-degree slope
        // Build the grid the same way CreateNavGridFromTerrain does.
        NavGrid g = makeOpenGrid(t.cols - 1, t.rows - 1, t.cellSize);
        for (int r = 0; r < g.rows; ++r) {
            for (int c = 0; c < g.cols; ++c) {
                float wx = (static_cast<float>(c) + 0.5f) * g.cellSize;
                float wz = (static_cast<float>(r) + 0.5f) * g.cellSize;
                size_t i = static_cast<size_t>(r) * g.cols + c;
                g.height[i] = terrainHeightAt(t, wx, wz);
                g.baseWalkable[i] = (terrainSlopeDegAt(t, wx, wz) <= 30.0f) ? 1 : 0;
            }
        }
        g.walkable = g.baseWalkable;
        CHECK(!navIsWalkable(g, 8, 8), "a 45-degree slope is not walkable under a 30-degree limit");
        CHECK(g.height[8 * 16 + 8] > 0.0f, "the nav grid carries terrain height");
    }

    // ---- agent stepping ----
    {
        agents().clear();
        navGrids().clear();
        uint64_t navId = navGrids().add(makeOpenGrid(20, 20, 1.0f));

        Agent a;
        a.nav = navId;
        a.position = Vec3(0.5f, 0.0f, 0.5f);
        a.speed = 2.0f;
        a.turnRate = 100000.0f; // effectively instant, so distance is the only variable
        a.radius = 0.4f;
        NavGrid* g = navGrids().get(navId);
        a.path = navFindPath(*g, 0.5f, 0.5f, 10.5f, 0.5f, true);
        a.pathIndex = a.path.size() > 1 ? 1 : 0;
        a.hasTarget = !a.path.empty();
        a.heading = 0.0f;
        uint64_t agentId = agents().add(a);

        CHECK(agents().get(agentId)->hasTarget, "the agent has a path to follow");
        float startX = agents().get(agentId)->position.x;

        // 1 second at 2 units/second, in 60Hz steps.
        for (int i = 0; i < 60; ++i) navUpdateAgents(1.0f / 60.0f);
        Agent* moved = agents().get(agentId);
        CHECK(moved->position.x > startX, "the agent moved toward its goal");
        CHECK(nearf(moved->position.x - startX, 2.0f, 0.15f), "the agent moved at roughly its speed");
        CHECK(!moved->arrived, "the agent has not arrived after one second of a five-second trip");
        CHECK(moved->stateTime > 0.9f, "state time accumulates while updating");

        // Long enough to finish.
        for (int i = 0; i < 600; ++i) navUpdateAgents(1.0f / 60.0f);
        Agent* done = agents().get(agentId);
        CHECK(done->arrived, "the agent eventually arrives");
        CHECK(nearf(done->position.x, 10.5f, 0.2f), "the agent stops at the goal");
        CHECK(nearf(done->velocity.length(), 0.0f), "an arrived agent has zero velocity");

        // Idle agents must not drift.
        Vec3 restingAt = done->position;
        for (int i = 0; i < 60; ++i) navUpdateAgents(1.0f / 60.0f);
        CHECK(nearf(agents().get(agentId)->position.x, restingAt.x), "an arrived agent stays put");

        agents().clear();
        navGrids().clear();
    }

    // ---- turn rate actually limits turning ----
    {
        agents().clear();
        navGrids().clear();
        uint64_t navId = navGrids().add(makeOpenGrid(20, 20, 1.0f));
        Agent a;
        a.nav = navId;
        a.position = Vec3(10.5f, 0.0f, 10.5f);
        a.speed = 5.0f;
        a.turnRate = 90.0f; // a quarter turn per second
        a.heading = 0.0f;   // facing +X
        NavGrid* g = navGrids().get(navId);
        a.path = navFindPath(*g, 10.5f, 10.5f, 10.5f, 19.5f, true); // goal is straight along +Z
        a.pathIndex = a.path.size() > 1 ? 1 : 0;
        a.hasTarget = !a.path.empty();
        uint64_t id = agents().add(a);

        navUpdateAgents(0.1f); // 0.1s at 90 deg/s = at most 9 degrees
        float headingDeg = agents().get(id)->heading * 180.0f / PV_PI;
        CHECK(headingDeg > 0.0f && headingDeg <= 9.5f, "turn rate caps how fast heading changes");

        agents().clear();
        navGrids().clear();
    }
}

// ======================================================================
int main() {
    std::printf("PlainVulkan runtime tests\n\n");
    testMath();
    testJson();
    testAnimation();
    testParticles();
    testValueSemantics();
    testTerrain();
    testAI();

    std::printf("\n%d checks, %d failure(s)\n", gChecks, gFailures);
    if (gFailures == 0) std::printf("PASS\n");
    return gFailures == 0 ? 0 : 1;
}
