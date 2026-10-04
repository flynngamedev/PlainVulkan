// debug.cpp -- Pv::DrawDebugLine/Box/Sphere + Log/LogWarning/LogError/
// Assert.
//
// The debug 3D shapes are implemented by projecting world-space points to
// screen space on the CPU (using the same view/projection the 3D pipeline
// uses) and drawing ordinary 2D lines -- genuinely functional overlays,
// reusing DrawLine rather than needing a dedicated line-list Vulkan
// pipeline. Simplification: no near-plane clipping, so an edge with one
// endpoint behind the camera is just skipped rather than clipped (see
// runtime/README.md).
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

#include <cmath>
#include <cstdlib>

namespace pv {

static bool projectToScreen(Vec3 world, float& outX, float& outY) {
    auto& e = engine();
    Mat4 vp = buildProjectionMatrix() * buildViewMatrix();
    float x = world.x, y = world.y, z = world.z;
    float clipX = vp.m[0][0] * x + vp.m[1][0] * y + vp.m[2][0] * z + vp.m[3][0];
    float clipY = vp.m[0][1] * x + vp.m[1][1] * y + vp.m[2][1] * z + vp.m[3][1];
    float clipW = vp.m[0][3] * x + vp.m[1][3] * y + vp.m[2][3] * z + vp.m[3][3];
    if (clipW <= 1e-4f) return false; // behind the camera
    float ndcX = clipX / clipW;
    float ndcY = clipY / clipW;
    outX = (ndcX * 0.5f + 0.5f) * static_cast<float>(e.swapchainExtent.width);
    outY = (ndcY * 0.5f + 0.5f) * static_cast<float>(e.swapchainExtent.height);
    return true;
}

static void drawProjectedLine(Vec3 a, Vec3 b, const Value& color) {
    float ax, ay, bx, by;
    if (!projectToScreen(a, ax, ay) || !projectToScreen(b, bx, by)) return;
    rt::DrawLine(Value(static_cast<double>(ax)), Value(static_cast<double>(ay)), Value(static_cast<double>(bx)),
                 Value(static_cast<double>(by)), color);
}

namespace rt {

Value DrawDebugLine(const Value& x1, const Value& y1, const Value& z1, const Value& x2, const Value& y2,
                     const Value& z2, const Value& color) {
    Vec3 a(static_cast<float>(x1.asFloat()), static_cast<float>(y1.asFloat()), static_cast<float>(z1.asFloat()));
    Vec3 b(static_cast<float>(x2.asFloat()), static_cast<float>(y2.asFloat()), static_cast<float>(z2.asFloat()));
    drawProjectedLine(a, b, color);
    return Value();
}

Value DrawDebugBox(const Value& x, const Value& y, const Value& z, const Value& w, const Value& h, const Value& d,
                    const Value& color) {
    float X = static_cast<float>(x.asFloat()), Y = static_cast<float>(y.asFloat()), Z = static_cast<float>(z.asFloat());
    float hw = static_cast<float>(w.asFloat()) * 0.5f, hh = static_cast<float>(h.asFloat()) * 0.5f,
          hd = static_cast<float>(d.asFloat()) * 0.5f;
    Vec3 c[8];
    int n = 0;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            for (int sz = -1; sz <= 1; sz += 2) c[n++] = Vec3(X + sx * hw, Y + sy * hh, Z + sz * hd);
    // corner index bit layout: bit0=x, bit1=y, bit2=z
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                      {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto& e : edges) drawProjectedLine(c[e[0]], c[e[1]], color);
    return Value();
}

Value DrawDebugSphere(const Value& x, const Value& y, const Value& z, const Value& radius, const Value& color) {
    float X = static_cast<float>(x.asFloat()), Y = static_cast<float>(y.asFloat()), Z = static_cast<float>(z.asFloat());
    float R = static_cast<float>(radius.asFloat());
    const int seg = 16;
    for (int i = 0; i < seg; i++) {
        float a0 = (2.0f * 3.14159265f * i) / seg;
        float a1 = (2.0f * 3.14159265f * (i + 1)) / seg;
        // three orthogonal great circles
        drawProjectedLine(Vec3(X + cosf(a0) * R, Y + sinf(a0) * R, Z), Vec3(X + cosf(a1) * R, Y + sinf(a1) * R, Z),
                           color);
        drawProjectedLine(Vec3(X + cosf(a0) * R, Y, Z + sinf(a0) * R), Vec3(X + cosf(a1) * R, Y, Z + sinf(a1) * R),
                           color);
        drawProjectedLine(Vec3(X, Y + cosf(a0) * R, Z + sinf(a0) * R), Vec3(X, Y + cosf(a1) * R, Z + sinf(a1) * R),
                           color);
    }
    return Value();
}

Value Log(const Value& message) {
    logLine("INFO", message.asString());
    return Value();
}
Value LogWarning(const Value& message) {
    logLine("WARNING", message.asString());
    return Value();
}
Value LogError(const Value& message) {
    logLine("ERROR", message.asString());
    return Value();
}
Value Assert(const Value& condition, const Value& message) {
    if (!condition.truthy()) {
        logLine("ERROR", "Assertion failed: " + message.asString());
    }
    return Value();
}

} // namespace rt
} // namespace pv
