// pv_math.cpp -- implementations for pv_math.h.
//
// The Mat4 basics (translate/scale/rotateXYZ/perspective/orthographic/
// lookAt/operator*/fromBasis) were lifted verbatim out of vk_core.cpp when
// the math grew past "just enough for view and projection matrices" and
// earned its own translation unit. Their behaviour is unchanged -- in
// particular perspective() still flips Y for Vulkan's NDC, so no existing
// rendering code needed touching. Everything below fromBasis() is new.
#include "pv/pv_math.h"

#include <cstring>

namespace pv {

Mat4 Mat4::translate(Vec3 t) {
    Mat4 m;
    m.m[3][0] = t.x;
    m.m[3][1] = t.y;
    m.m[3][2] = t.z;
    return m;
}

Mat4 Mat4::scale(Vec3 s) {
    Mat4 m;
    m.m[0][0] = s.x;
    m.m[1][1] = s.y;
    m.m[2][2] = s.z;
    return m;
}

Mat4 Mat4::rotateXYZ(float pitch, float yaw, float roll) {
    float cx = std::cos(pitch), sx = std::sin(pitch);
    float cy = std::cos(yaw), sy = std::sin(yaw);
    float cz = std::cos(roll), sz = std::sin(roll);
    Mat4 rx, ry, rz;
    rx.m[1][1] = cx; rx.m[1][2] = sx; rx.m[2][1] = -sx; rx.m[2][2] = cx;
    ry.m[0][0] = cy; ry.m[0][2] = -sy; ry.m[2][0] = sy; ry.m[2][2] = cy;
    rz.m[0][0] = cz; rz.m[0][1] = sz; rz.m[1][0] = -sz; rz.m[1][1] = cz;
    return rz * ry * rx;
}

Mat4 Mat4::fromQuat(const Quat& q) {
    // Standard unit-quaternion -> rotation matrix. Normalizing first means
    // a slightly-drifted quaternion (inevitable after many incremental
    // multiplications during animation blending) produces a pure rotation
    // rather than a rotation-with-scale, which would make meshes subtly
    // grow or shrink over time.
    Quat n = q.normalized();
    float xx = n.x * n.x, yy = n.y * n.y, zz = n.z * n.z;
    float xy = n.x * n.y, xz = n.x * n.z, yz = n.y * n.z;
    float wx = n.w * n.x, wy = n.w * n.y, wz = n.w * n.z;

    Mat4 m = Mat4::identity();
    m.m[0][0] = 1.0f - 2.0f * (yy + zz);
    m.m[0][1] = 2.0f * (xy + wz);
    m.m[0][2] = 2.0f * (xz - wy);

    m.m[1][0] = 2.0f * (xy - wz);
    m.m[1][1] = 1.0f - 2.0f * (xx + zz);
    m.m[1][2] = 2.0f * (yz + wx);

    m.m[2][0] = 2.0f * (xz + wy);
    m.m[2][1] = 2.0f * (yz - wx);
    m.m[2][2] = 1.0f - 2.0f * (xx + yy);
    return m;
}

Mat4 Mat4::fromTRS(const Vec3& t, const Quat& r, const Vec3& s) {
    // Builds T * R * S directly instead of multiplying three matrices --
    // this runs once per entity per frame in the scene draw loop, and the
    // fused form skips 128 multiply-adds of mostly-zero work.
    Mat4 m = fromQuat(r);
    m.m[0][0] *= s.x; m.m[0][1] *= s.x; m.m[0][2] *= s.x;
    m.m[1][0] *= s.y; m.m[1][1] *= s.y; m.m[1][2] *= s.y;
    m.m[2][0] *= s.z; m.m[2][1] *= s.z; m.m[2][2] *= s.z;
    m.m[3][0] = t.x;
    m.m[3][1] = t.y;
    m.m[3][2] = t.z;
    return m;
}

Mat4 Mat4::perspective(float fovYRadians, float aspect, float nearZ, float farZ) {
    Mat4 m{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) m.m[i][j] = 0;
    float f = 1.0f / std::tan(fovYRadians * 0.5f);
    m.m[0][0] = f / aspect;
    // Vulkan NDC Y points down, so flip Y here rather than at every call site.
    m.m[1][1] = -f;
    m.m[2][2] = farZ / (nearZ - farZ);
    m.m[2][3] = -1.0f;
    m.m[3][2] = (farZ * nearZ) / (nearZ - farZ);
    return m;
}

Mat4 Mat4::orthographic(float left, float right, float bottom, float top, float nearZ, float farZ) {
    Mat4 m = Mat4::identity();
    m.m[0][0] = 2.0f / (right - left);
    m.m[1][1] = -2.0f / (top - bottom); // flip Y for Vulkan NDC
    m.m[2][2] = 1.0f / (nearZ - farZ);
    m.m[3][0] = -(right + left) / (right - left);
    m.m[3][1] = -(top + bottom) / (top - bottom);
    m.m[3][2] = nearZ / (nearZ - farZ);
    return m;
}

Mat4 Mat4::lookAt(Vec3 eye, Vec3 center, Vec3 up) {
    Vec3 f = (center - eye).normalized();
    Vec3 s = cross(f, up).normalized();
    Vec3 u = cross(s, f);
    Mat4 m = Mat4::identity();
    m.m[0][0] = s.x; m.m[1][0] = s.y; m.m[2][0] = s.z;
    m.m[0][1] = u.x; m.m[1][1] = u.y; m.m[2][1] = u.z;
    m.m[0][2] = -f.x; m.m[1][2] = -f.y; m.m[2][2] = -f.z;
    m.m[3][0] = -dot(s, eye);
    m.m[3][1] = -dot(u, eye);
    m.m[3][2] = dot(f, eye);
    return m;
}

Mat4 Mat4::operator*(const Mat4& o) const {
    Mat4 r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++) sum += m[k][row] * o.m[col][k];
            r.m[col][row] = sum;
        }
    }
    return r;
}

Vec4 Mat4::operator*(const Vec4& v) const {
    return {m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z + m[3][0] * v.w,
            m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z + m[3][1] * v.w,
            m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z + m[3][2] * v.w,
            m[0][3] * v.x + m[1][3] * v.y + m[2][3] * v.z + m[3][3] * v.w};
}

Mat4 Mat4::fromBasis(Vec3 right, Vec3 up, Vec3 forward, Vec3 translation) {
    Mat4 m = Mat4::identity();
    m.m[0][0] = right.x; m.m[0][1] = right.y; m.m[0][2] = right.z;
    m.m[1][0] = up.x; m.m[1][1] = up.y; m.m[1][2] = up.z;
    m.m[2][0] = forward.x; m.m[2][1] = forward.y; m.m[2][2] = forward.z;
    m.m[3][0] = translation.x; m.m[3][1] = translation.y; m.m[3][2] = translation.z;
    return m;
}

Mat4 Mat4::transposed() const {
    Mat4 r;
    for (int c = 0; c < 4; c++)
        for (int rw = 0; rw < 4; rw++) r.m[c][rw] = m[rw][c];
    return r;
}

Mat4 Mat4::inverse() const {
    // Full 4x4 cofactor inverse. A cheaper affine-only inverse would cover
    // the scene-graph cases, but the editor also inverts the *projection*
    // matrix for mouse picking, and a perspective matrix is not affine --
    // so the general form is the one that's actually needed.
    const float* a = &m[0][0]; // column-major flat: a[col*4 + row]

    float s0 = a[0] * a[5] - a[4] * a[1];
    float s1 = a[0] * a[9] - a[8] * a[1];
    float s2 = a[0] * a[13] - a[12] * a[1];
    float s3 = a[4] * a[9] - a[8] * a[5];
    float s4 = a[4] * a[13] - a[12] * a[5];
    float s5 = a[8] * a[13] - a[12] * a[9];

    float c5 = a[10] * a[15] - a[14] * a[11];
    float c4 = a[6] * a[15] - a[14] * a[7];
    float c3 = a[6] * a[11] - a[10] * a[7];
    float c2 = a[2] * a[15] - a[14] * a[3];
    float c1 = a[2] * a[11] - a[10] * a[3];
    float c0 = a[2] * a[7] - a[6] * a[3];

    float det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (std::fabs(det) < 1e-12f) {
        // Singular. Returning identity is the least-bad option: it keeps
        // the caller's arithmetic finite (an inf/NaN matrix would silently
        // corrupt every downstream transform) and shows up as an obviously
        // wrong result rather than an invisible one.
        return Mat4::identity();
    }
    float invdet = 1.0f / det;

    Mat4 r;
    float* b = &r.m[0][0];
    b[0]  = ( a[5] * c5 - a[9] * c4 + a[13] * c3) * invdet;
    b[1]  = (-a[1] * c5 + a[9] * c2 - a[13] * c1) * invdet;
    b[2]  = ( a[1] * c4 - a[5] * c2 + a[13] * c0) * invdet;
    b[3]  = (-a[1] * c3 + a[5] * c1 - a[9]  * c0) * invdet;

    b[4]  = (-a[4] * c5 + a[8] * c4 - a[12] * c3) * invdet;
    b[5]  = ( a[0] * c5 - a[8] * c2 + a[12] * c1) * invdet;
    b[6]  = (-a[0] * c4 + a[4] * c2 - a[12] * c0) * invdet;
    b[7]  = ( a[0] * c3 - a[4] * c1 + a[8]  * c0) * invdet;

    b[8]  = ( a[7] * s5 - a[11] * s4 + a[15] * s3) * invdet;
    b[9]  = (-a[3] * s5 + a[11] * s2 - a[15] * s1) * invdet;
    b[10] = ( a[3] * s4 - a[7]  * s2 + a[15] * s0) * invdet;
    b[11] = (-a[3] * s3 + a[7]  * s1 - a[11] * s0) * invdet;

    b[12] = (-a[6] * s5 + a[10] * s4 - a[14] * s3) * invdet;
    b[13] = ( a[2] * s5 - a[10] * s2 + a[14] * s1) * invdet;
    b[14] = (-a[2] * s4 + a[6]  * s2 - a[14] * s0) * invdet;
    b[15] = ( a[2] * s3 - a[6]  * s1 + a[10] * s0) * invdet;
    return r;
}

Vec3 Mat4::transformPoint(const Vec3& p) const {
    Vec4 r = (*this) * Vec4(p, 1.0f);
    // Perspective divide when w != 1, so this works for both affine
    // transforms (w stays 1, divide is a no-op) and projection matrices.
    if (std::fabs(r.w) > 1e-8f && std::fabs(r.w - 1.0f) > 1e-8f) {
        return {r.x / r.w, r.y / r.w, r.z / r.w};
    }
    return {r.x, r.y, r.z};
}

Vec3 Mat4::transformDirection(const Vec3& d) const {
    Vec4 r = (*this) * Vec4(d, 0.0f);
    return {r.x, r.y, r.z};
}

void decomposeTRS(const Mat4& mat, Vec3& outT, Quat& outR, Vec3& outS) {
    outT = Vec3(mat.m[3][0], mat.m[3][1], mat.m[3][2]);

    Vec3 cx(mat.m[0][0], mat.m[0][1], mat.m[0][2]);
    Vec3 cy(mat.m[1][0], mat.m[1][1], mat.m[1][2]);
    Vec3 cz(mat.m[2][0], mat.m[2][1], mat.m[2][2]);

    outS = Vec3(cx.length(), cy.length(), cz.length());

    // A negative determinant means the matrix mirrors. A rotation
    // quaternion can't represent a mirror, so fold the flip into the X
    // scale -- the standard convention, and it round-trips correctly
    // through fromTRS.
    Mat4 rotOnly = Mat4::identity();
    float det = dot(cx, cross(cy, cz));
    if (det < 0.0f) {
        outS.x = -outS.x;
        cx = -cx;
    }
    if (outS.x > 1e-8f) cx = cx / outS.x;
    if (outS.y > 1e-8f) cy = cy / std::fabs(outS.y);
    if (outS.z > 1e-8f) cz = cz / std::fabs(outS.z);

    rotOnly.m[0][0] = cx.x; rotOnly.m[0][1] = cx.y; rotOnly.m[0][2] = cx.z;
    rotOnly.m[1][0] = cy.x; rotOnly.m[1][1] = cy.y; rotOnly.m[1][2] = cy.z;
    rotOnly.m[2][0] = cz.x; rotOnly.m[2][1] = cz.y; rotOnly.m[2][2] = cz.z;

    // Shepperd's method: pick the largest diagonal term to divide by, which
    // avoids the catastrophic precision loss the naive trace-only formula
    // suffers when the trace approaches zero (a 180-degree rotation).
    const float(*r)[4] = rotOnly.m;
    float trace = r[0][0] + r[1][1] + r[2][2];
    Quat q;
    if (trace > 0.0f) {
        float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (r[1][2] - r[2][1]) / s;
        q.y = (r[2][0] - r[0][2]) / s;
        q.z = (r[0][1] - r[1][0]) / s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
        q.w = (r[1][2] - r[2][1]) / s;
        q.x = 0.25f * s;
        q.y = (r[1][0] + r[0][1]) / s;
        q.z = (r[2][0] + r[0][2]) / s;
    } else if (r[1][1] > r[2][2]) {
        float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
        q.w = (r[2][0] - r[0][2]) / s;
        q.x = (r[1][0] + r[0][1]) / s;
        q.y = 0.25f * s;
        q.z = (r[2][1] + r[1][2]) / s;
    } else {
        float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
        q.w = (r[0][1] - r[1][0]) / s;
        q.x = (r[2][0] + r[0][2]) / s;
        q.y = (r[2][1] + r[1][2]) / s;
        q.z = 0.25f * s;
    }
    outR = q.normalized();
}

// ----------------------------------------------------------------------
// Path helpers declared in pv_platform.h. They live here rather than in
// vk_core.cpp so that tools which link only the math/scene layer (the
// scene-format unit tests) don't drag in Vulkan.
// ----------------------------------------------------------------------
std::string normalizeSlashes(std::string path) {
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    return path;
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::string out = a;
    bool aEnds = (out.back() == '/' || out.back() == '\\');
    bool bStarts = (b.front() == '/' || b.front() == '\\');
    if (aEnds && bStarts) return out + b.substr(1);
    if (!aEnds && !bStarts) return out + "/" + b;
    return out + b;
}

} // namespace pv
