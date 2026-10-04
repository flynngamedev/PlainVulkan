// pv_math.h -- the runtime's small math library. Split out of
// pv_internal.h because three new subsystems (PhysX interop, keyframe
// animation, and the editor's transform gizmo) all need rotation as a
// *quaternion* rather than the yaw-only float the old animation path
// used, and all three need to compose/decompose a full TRS transform.
//
// Conventions, stated once so nothing downstream has to guess:
//   - Right-handed, Y-up, -Z forward (matching glTF and the existing
//     camera code in camera.cpp).
//   - Mat4 is column-major, m[col][row] -- GLSL/Vulkan layout, so a Mat4
//     can be memcpy'd straight into a uniform buffer.
//   - Quat is (x, y, z, w) with w last, matching glTF's accessor layout
//     and PhysX's PxQuat, so animation import and physics interop are
//     both plain copies with no component shuffling.
//   - Angles are radians internally. Only the public Pv:: API and the
//     editor take degrees, and both convert at the boundary.
#pragma once

#include "pv/pv_platform.h"

#include <algorithm>
#include <cmath>

namespace pv {

// ------------------------------------------------------------------
// Vec3
// ------------------------------------------------------------------
struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    explicit Vec3(float s) : x(s), y(s), z(s) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    // Component-wise, used for non-uniform scale and particle color tint.
    Vec3 operator*(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }

    float lengthSq() const { return x * x + y * y + z * z; }
    float length() const { return std::sqrt(lengthSq()); }
    Vec3 normalized() const {
        float l = length();
        // Returning zero rather than NaN keeps a degenerate direction from
        // poisoning a whole frame of transforms; callers that care check
        // lengthSq() first.
        return l > 1e-8f ? (*this / l) : Vec3(0, 0, 0);
    }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline Vec3 minv(const Vec3& a, const Vec3& b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
inline Vec3 maxv(const Vec3& a, const Vec3& b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

// ------------------------------------------------------------------
// Vec4 -- used for colors (particles interpolate RGBA over lifetime) and
// for homogeneous points when projecting to screen space.
// ------------------------------------------------------------------
struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    Vec4() = default;
    Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vec4(const Vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    Vec4 operator+(const Vec4& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    Vec4 operator-(const Vec4& o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    Vec4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
    Vec3 xyz() const { return {x, y, z}; }
};
inline Vec4 lerp(const Vec4& a, const Vec4& b, float t) { return a + (b - a) * t; }

// ------------------------------------------------------------------
// Quat -- (x, y, z, w), w last. Unit quaternions represent rotation.
// ------------------------------------------------------------------
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    Quat() = default;
    Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    static Quat identity() { return {0, 0, 0, 1}; }

    static Quat fromAxisAngle(const Vec3& axis, float radians) {
        Vec3 a = axis.normalized();
        float h = radians * 0.5f;
        float s = std::sin(h);
        return {a.x * s, a.y * s, a.z * s, std::cos(h)};
    }

    // Tait-Bryan angles composed as Rz(roll) * Ry(yaw) * Rx(pitch) -- i.e.
    // X is applied to the vector first, then Y, then Z. That is exactly
    // what Mat4::rotateXYZ has always computed (it returns `rz * ry * rx`),
    // so every existing .pv script calling RotateCamera or DrawMeshEx keeps
    // its current behaviour now that those go through quaternions.
    //
    // The expanded product below is qz * qy * qx worked out by hand; the
    // sign pattern is easy to get subtly wrong, so pv_math_tests.cpp checks
    // it against both rotateXYZ and toEuler.
    static Quat fromEuler(float pitch, float yaw, float roll) {
        float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
        float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
        float cr = std::cos(roll * 0.5f), sr = std::sin(roll * 0.5f);
        return {cy * sp * cr - sy * cp * sr,
                sy * cp * cr + cy * sp * sr,
                cy * cp * sr - sy * sp * cr,
                cy * cp * cr + sy * sp * sr};
    }

    Vec3 toEuler() const {
        Vec3 e;
        // Pitch, with the standard gimbal-lock guard: asin's domain is
        // [-1, 1] and accumulated float error can push the argument just
        // outside it, which would return NaN and corrupt the inspector's
        // displayed rotation.
        float sinp = 2.0f * (w * x + y * z);
        float cosp = 1.0f - 2.0f * (x * x + y * y);
        e.x = std::atan2(sinp, cosp);
        float siny = 2.0f * (w * y - z * x);
        siny = std::max(-1.0f, std::min(1.0f, siny));
        e.y = std::asin(siny);
        float sinr = 2.0f * (w * z + x * y);
        float cosr = 1.0f - 2.0f * (y * y + z * z);
        e.z = std::atan2(sinr, cosr);
        return e;
    }

    Quat operator*(const Quat& o) const {
        return {w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x,
                w * o.z + x * o.y - y * o.x + z * o.w,
                w * o.w - x * o.x - y * o.y - z * o.z};
    }
    Quat operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
    Quat operator+(const Quat& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    Quat operator-() const { return {-x, -y, -z, -w}; }

    Quat conjugate() const { return {-x, -y, -z, w}; }
    float lengthSq() const { return x * x + y * y + z * z + w * w; }
    float length() const { return std::sqrt(lengthSq()); }
    Quat normalized() const {
        float l = length();
        return l > 1e-8f ? Quat{x / l, y / l, z / l, w / l} : identity();
    }

    // Rotate a vector: v' = q * v * q^-1, expanded to avoid building two
    // temporary quaternions per call (this runs per-particle per-frame).
    Vec3 rotate(const Vec3& v) const {
        Vec3 u{x, y, z};
        float s = w;
        return u * (2.0f * dot(u, v)) + v * (s * s - u.lengthSq()) + cross(u, v) * (2.0f * s);
    }
};

inline float dot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

// Normalized linear interpolation. Cheaper than slerp and visually
// indistinguishable for the small per-frame steps keyframe sampling takes.
inline Quat nlerp(const Quat& a, Quat b, float t) {
    // q and -q are the same rotation; without this check, interpolating
    // between two keyframes stored with opposite signs takes the long way
    // around the sphere -- the classic "limb spins 350 degrees backwards"
    // animation bug.
    if (dot(a, b) < 0.0f) b = -b;
    return (a * (1.0f - t) + b * t).normalized();
}

// True spherical interpolation. Used for animation blending between two
// clips, where the angular distance can be large enough for nlerp's
// non-constant angular velocity to be visible.
inline Quat slerp(const Quat& a, Quat b, float t) {
    float d = dot(a, b);
    if (d < 0.0f) { b = -b; d = -d; }
    if (d > 0.9995f) return nlerp(a, b, t); // near-parallel: sin(theta) -> 0, slerp is numerically unstable
    float theta = std::acos(std::max(-1.0f, std::min(1.0f, d)));
    float sinTheta = std::sin(theta);
    float wa = std::sin((1.0f - t) * theta) / sinTheta;
    float wb = std::sin(t * theta) / sinTheta;
    return (a * wa + b * wb).normalized();
}

// ------------------------------------------------------------------
// Mat4 -- column-major, m[col][row].
// ------------------------------------------------------------------
struct Mat4 {
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    static Mat4 identity() { return Mat4(); }
    static Mat4 translate(Vec3 t);
    static Mat4 scale(Vec3 s);
    static Mat4 rotateXYZ(float pitch, float yaw, float roll); // radians
    static Mat4 fromQuat(const Quat& q);
    // Composes T * R * S in that order -- the standard glTF/scene-graph
    // convention, so scale is applied first in the object's own space.
    static Mat4 fromTRS(const Vec3& t, const Quat& r, const Vec3& s);
    static Mat4 perspective(float fovYRadians, float aspect, float nearZ, float farZ);
    static Mat4 orthographic(float left, float right, float bottom, float top, float nearZ, float farZ);
    static Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up);
    static Mat4 fromBasis(Vec3 right, Vec3 up, Vec3 forward, Vec3 translation);

    Mat4 operator*(const Mat4& o) const;
    Vec4 operator*(const Vec4& v) const;

    // General inverse. Needed by the editor's mouse-picking (screen ray ->
    // world ray needs inverse(proj * view)) and by scene-graph parenting
    // (world -> local when re-parenting an entity without moving it).
    Mat4 inverse() const;
    Mat4 transposed() const;

    Vec3 transformPoint(const Vec3& p) const;    // w = 1, includes translation
    Vec3 transformDirection(const Vec3& d) const; // w = 0, ignores translation
};

// Splits a matrix back into translation / rotation / scale. Lossy for
// sheared or negatively-scaled matrices (both of which a TRS triple
// fundamentally can't express), which is exactly why the scene format
// stores TRS directly and only decomposes when importing foreign data.
void decomposeTRS(const Mat4& mat, Vec3& outT, Quat& outR, Vec3& outS);

// ------------------------------------------------------------------
// Transform -- the scene graph's per-entity local transform.
// ------------------------------------------------------------------
struct Transform {
    Vec3 position{0, 0, 0};
    Quat rotation = Quat::identity();
    Vec3 scale{1, 1, 1};

    Mat4 matrix() const { return Mat4::fromTRS(position, rotation, scale); }

    static Transform lerped(const Transform& a, const Transform& b, float t) {
        Transform r;
        r.position = lerp(a.position, b.position, t);
        r.rotation = nlerp(a.rotation, b.rotation, t);
        r.scale = lerp(a.scale, b.scale, t);
        return r;
    }
};

// ------------------------------------------------------------------
// Misc scalar helpers
// ------------------------------------------------------------------
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Deterministic per-emitter PRNG. std::mt19937 is 2.5KB of state per
// emitter and std::rand() is global mutable state shared with the rest of
// the process (so adding an emitter would perturb every other random
// consumer). xorshift32 is 4 bytes, fast, and reproducible from a seed --
// which matters because the editor needs to scrub a particle effect back
// and forth and see the same result.
struct Rng {
    uint32_t state = 0x9E3779B9u;
    explicit Rng(uint32_t seed = 0x9E3779B9u) : state(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    float unit() { return static_cast<float>(next() & 0xFFFFFF) / static_cast<float>(0x1000000); } // [0,1)
    float range(float lo, float hi) { return lo + unit() * (hi - lo); }
    Vec3 insideUnitSphere() {
        // Rejection sampling: uniform in the cube, retry until inside the
        // sphere. Average 1.9 iterations, and unlike normalizing a
        // Gaussian it needs no transcendentals.
        for (int i = 0; i < 8; i++) {
            Vec3 v{range(-1, 1), range(-1, 1), range(-1, 1)};
            if (v.lengthSq() <= 1.0f) return v;
        }
        return Vec3(0, 0, 0);
    }
    Vec3 onUnitSphere() {
        Vec3 v = insideUnitSphere();
        return v.lengthSq() > 1e-6f ? v.normalized() : Vec3(0, 1, 0);
    }
};

} // namespace pv
