// gizmo.cpp -- viewport mouse picking and the translate/rotate/scale
// manipulator.
//
// Drawn with ImGui's foreground draw list rather than as engine geometry.
// That's a deliberate choice: a gizmo has to be visible *through* the
// objects it manipulates (otherwise you can't grab a handle on something
// half-buried in the floor) and must never be affected by scene lighting or
// depth. Rendering it as 3D geometry would mean a second pipeline with
// depth-test off and its own unlit shader; projecting to screen space and
// letting ImGui draw the lines gets the same result with none of that.
//
// The projection maths is shared with picking, which is why both live here.
#include "gizmo.h"

// Only the scene/math layer is needed here -- deliberately not
// pv_internal.h, which would pull in Vulkan and make this file (and its
// picking maths) untestable without a GPU.
#include "pv/pv_scene.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace pv {
namespace gizmo {

namespace {

// Projects a world point to pixel coordinates. Returns false when the point
// is behind the camera -- callers must check, because a behind-camera point
// projects to a mirrored position on screen and would draw a handle in
// completely the wrong place.
bool worldToScreen(const Mat4& viewProj, const Vec3& world, float screenW, float screenH, ImVec2& out) {
    Vec4 clip = viewProj * Vec4(world, 1.0f);
    if (clip.w <= 1e-5f) return false;
    float ndcX = clip.x / clip.w;
    float ndcY = clip.y / clip.w;
    // Vulkan NDC is already Y-down (the projection matrix flips it), which
    // matches ImGui's top-left pixel origin, so no second flip here.
    out = ImVec2((ndcX * 0.5f + 0.5f) * screenW, (ndcY * 0.5f + 0.5f) * screenH);
    return true;
}

// Shortest distance from a point to a line segment, in pixels. Used for
// hit-testing an axis handle, which is far more forgiving than requiring a
// click on the exact line.
float distanceToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    float vx = b.x - a.x, vy = b.y - a.y;
    float wx = p.x - a.x, wy = p.y - a.y;
    float len2 = vx * vx + vy * vy;
    if (len2 < 1e-6f) return std::sqrt(wx * wx + wy * wy);
    float t = clampf((wx * vx + wy * vy) / len2, 0.0f, 1.0f);
    float dx = wx - t * vx, dy = wy - t * vy;
    return std::sqrt(dx * dx + dy * dy);
}

// Closest point on the world-space axis ray to the camera ray through the
// mouse. This is what turns a 2D mouse position into a 1D position along
// the dragged axis -- the standard closest-approach-of-two-lines solve.
//
// Returns false when the two rays are near-parallel, which happens when you
// look straight down an axis. Bailing out there rather than dividing by a
// near-zero denominator is what stops the object shooting off to infinity
// in that view.
bool closestPointOnAxis(const Vec3& axisOrigin, const Vec3& axisDir, const Vec3& rayOrigin,
                        const Vec3& rayDir, float& outT) {
    Vec3 w0 = axisOrigin - rayOrigin;
    float a = dot(axisDir, axisDir);
    float b = dot(axisDir, rayDir);
    float c = dot(rayDir, rayDir);
    float d = dot(axisDir, w0);
    float e = dot(rayDir, w0);
    float denom = a * c - b * b;
    if (std::fabs(denom) < 1e-6f) return false;
    outT = (b * e - c * d) / denom;
    return true;
}

const ImU32 kAxisColor[3] = {
    IM_COL32(230, 70, 70, 255),  // X
    IM_COL32(110, 220, 90, 255), // Y
    IM_COL32(90, 150, 245, 255), // Z
};
const ImU32 kHighlight = IM_COL32(255, 220, 60, 255);

} // namespace

Ray screenRay(const Mat4& view, const Mat4& proj, float mouseX, float mouseY, float screenW, float screenH) {
    Ray r;
    // Pixel -> NDC. The Y term is not flipped for the same reason as
    // worldToScreen: the projection matrix already did it.
    float ndcX = (mouseX / screenW) * 2.0f - 1.0f;
    float ndcY = (mouseY / screenH) * 2.0f - 1.0f;

    Mat4 invVP = (proj * view).inverse();
    // Unproject two points at different depths and subtract. Doing it this
    // way (rather than building a direction from the FOV by hand) works
    // unchanged for an orthographic camera, where all rays are parallel and
    // the origin varies instead of the direction.
    Vec4 nearH = invVP * Vec4(ndcX, ndcY, 0.0f, 1.0f);
    Vec4 farH = invVP * Vec4(ndcX, ndcY, 1.0f, 1.0f);
    if (std::fabs(nearH.w) < 1e-8f || std::fabs(farH.w) < 1e-8f) {
        r.origin = Vec3();
        r.direction = Vec3(0, 0, -1);
        return r;
    }
    Vec3 nearP = Vec3(nearH.x / nearH.w, nearH.y / nearH.w, nearH.z / nearH.w);
    Vec3 farP = Vec3(farH.x / farH.w, farH.y / farH.w, farH.z / farH.w);
    r.origin = nearP;
    r.direction = (farP - nearP).normalized();
    return r;
}

uint64_t pickEntity(Scene& s, const Ray& ray) {
    // Ray-sphere against each entity's bounding sphere, nearest wins.
    //
    // A sphere rather than the mesh's real geometry: the runtime doesn't
    // keep CPU-side vertex data after upload (mesh.cpp streams it straight
    // to the GPU), so exact triangle picking would mean retaining a second
    // copy of every mesh purely for the editor. A bounding sphere sized
    // from the entity's scale is accurate enough to select the right object
    // in practice, and costs nothing.
    uint64_t best = 0;
    float bestT = 1e30f;

    for (const auto& kv : s.entities) {
        const Entity& e = kv.second;
        if (!e.active) continue;
        // Only things you can actually see are selectable; picking an
        // invisible logic entity by clicking empty space would be baffling.
        if (!e.has(COMP_MESH) && !e.has(COMP_LIGHT) && !e.has(COMP_EMITTER) && !e.has(COMP_CAMERA)) {
            continue;
        }

        Vec3 center(e.world.m[3][0], e.world.m[3][1], e.world.m[3][2]);
        float radius = 0.6f;
        if (e.has(COMP_MESH)) {
            // Largest scale component, so a stretched object stays clickable
            // along its long axis.
            float maxScale = std::max({std::fabs(e.local.scale.x), std::fabs(e.local.scale.y),
                                       std::fabs(e.local.scale.z)});
            radius = 0.9f * std::max(maxScale, 0.05f);
        }

        Vec3 m = ray.origin - center;
        float b = dot(m, ray.direction);
        float c = m.lengthSq() - radius * radius;
        if (c > 0.0f && b > 0.0f) continue; // outside and pointing away
        float disc = b * b - c;
        if (disc < 0.0f) continue;
        float t = -b - std::sqrt(disc);
        if (t < 0.0f) t = 0.0f;
        if (t < bestT) {
            bestT = t;
            best = kv.first;
        }
    }
    return best;
}

bool manipulate(Scene& s, uint64_t entityId, Mode mode, State& st, const Mat4& view, const Mat4& proj,
                float screenW, float screenH, bool mouseDown, bool mousePressed, float mouseX, float mouseY) {
    Entity* e = s.entities.get(entityId);
    if (!e) {
        st.dragging = false;
        return false;
    }

    Mat4 viewProj = proj * view;
    Vec3 origin(e->world.m[3][0], e->world.m[3][1], e->world.m[3][2]);

    ImVec2 originScreen;
    if (!worldToScreen(viewProj, origin, screenW, screenH, originScreen)) {
        st.dragging = false;
        return false;
    }

    // Axis directions. Local mode uses the entity's own orientation, which
    // is what you want when nudging a rotated object along its own length.
    Vec3 axes[3] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
    if (st.localSpace) {
        for (int i = 0; i < 3; i++) axes[i] = e->local.rotation.rotate(axes[i]).normalized();
    }

    // Constant screen-size handles: scale the world-space length by the
    // distance to the camera, so the gizmo stays the same pixel size whether
    // the object is next to you or far away. A fixed world length would make
    // it a dot at distance and swallow the screen up close.
    Vec3 camPos(view.inverse().m[3][0], view.inverse().m[3][1], view.inverse().m[3][2]);
    float distance = (origin - camPos).length();
    float handleLength = std::max(distance * 0.16f, 0.15f);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 mouse(mouseX, mouseY);

    ImVec2 tips[3];
    bool tipValid[3];
    for (int i = 0; i < 3; i++) {
        tipValid[i] = worldToScreen(viewProj, origin + axes[i] * handleLength, screenW, screenH, tips[i]);
    }

    // --- hit test ---
    int hovered = -1;
    if (!st.dragging) {
        float bestDist = 12.0f; // pixels of grab tolerance
        for (int i = 0; i < 3; i++) {
            if (!tipValid[i]) continue;
            float d = distanceToSegment(mouse, originScreen, tips[i]);
            if (d < bestDist) {
                bestDist = d;
                hovered = i;
            }
        }
    }

    // --- begin drag ---
    if (mousePressed && hovered >= 0 && !ImGui::GetIO().WantCaptureMouse) {
        st.dragging = true;
        st.axis = hovered;
        st.startTransform = e->local;

        Ray ray = screenRay(view, proj, mouseX, mouseY, screenW, screenH);
        float t = 0.0f;
        if (closestPointOnAxis(origin, axes[hovered], ray.origin, ray.direction, t)) {
            // Store the grab offset rather than the raw parameter, so the
            // object doesn't snap its handle to the cursor on mouse-down.
            st.dragStartParam = t;
        } else {
            st.dragging = false;
        }
    }

    // --- continue drag ---
    bool changed = false;
    if (st.dragging) {
        if (!mouseDown) {
            st.dragging = false;
        } else {
            Ray ray = screenRay(view, proj, mouseX, mouseY, screenW, screenH);
            float t = 0.0f;
            Vec3 axis = axes[st.axis];
            // The drag is measured against the transform as it was when the
            // drag began, not incrementally frame to frame. Incremental
            // accumulation drifts, and makes a drag that returns to its
            // starting point fail to restore the original value.
            Vec3 dragOrigin(0, 0, 0);
            if (st.dragging) {
                Mat4 parentWorld = Mat4::identity();
                if (e->parent != 0) {
                    if (Entity* p = s.entities.get(e->parent)) parentWorld = p->world;
                }
                dragOrigin = parentWorld.transformPoint(st.startTransform.position);
            }

            if (closestPointOnAxis(dragOrigin, axis, ray.origin, ray.direction, t)) {
                float delta = t - st.dragStartParam;

                if (st.snap > 0.0f) {
                    delta = std::round(delta / st.snap) * st.snap;
                }

                switch (mode) {
                    case Mode::Translate: {
                        Vec3 worldDelta = axis * delta;
                        // The entity stores a *local* transform, so a world
                        // delta has to be rotated into the parent's space or
                        // dragging a parented object moves it along the
                        // wrong axis.
                        if (e->parent != 0) {
                            if (Entity* p = s.entities.get(e->parent)) {
                                worldDelta = p->world.inverse().transformDirection(worldDelta);
                            }
                        }
                        e->local.position = st.startTransform.position + worldDelta;
                        changed = true;
                        break;
                    }
                    case Mode::Scale: {
                        Vec3 sc = st.startTransform.scale;
                        float amount = 1.0f + delta * 0.5f;
                        // Clamped away from zero: a zero or negative scale
                        // makes the world matrix singular, which propagates
                        // as an identity inverse to every child.
                        amount = std::max(amount, 0.01f);
                        if (st.axis == 0) sc.x = st.startTransform.scale.x * amount;
                        if (st.axis == 1) sc.y = st.startTransform.scale.y * amount;
                        if (st.axis == 2) sc.z = st.startTransform.scale.z * amount;
                        e->local.scale = sc;
                        changed = true;
                        break;
                    }
                    case Mode::Rotate: {
                        // Radians per world unit dragged. Scaled by handle
                        // length so the feel is the same at any zoom.
                        float angle = delta / std::max(handleLength, 1e-3f);
                        Vec3 localAxis = st.localSpace ? (st.axis == 0   ? Vec3(1, 0, 0)
                                                          : st.axis == 1 ? Vec3(0, 1, 0)
                                                                         : Vec3(0, 0, 1))
                                                       : axes[st.axis];
                        Quat spin = Quat::fromAxisAngle(localAxis, angle);
                        e->local.rotation = st.localSpace ? (st.startTransform.rotation * spin)
                                                          : (spin * st.startTransform.rotation);
                        e->local.rotation = e->local.rotation.normalized();
                        changed = true;
                        break;
                    }
                }
            }
        }
    }

    if (changed) {
        markWorldDirty(s, entityId);
        s.dirty = true;
    }

    // --- draw ---
    const char* labels[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; i++) {
        if (!tipValid[i]) continue;
        bool active = (st.dragging && st.axis == i) || (!st.dragging && hovered == i);
        ImU32 col = active ? kHighlight : kAxisColor[i];
        float thickness = active ? 3.5f : 2.0f;

        dl->AddLine(originScreen, tips[i], col, thickness);

        if (mode == Mode::Translate) {
            // A filled arrowhead, built from the on-screen direction so it
            // points correctly from any camera angle.
            ImVec2 d(tips[i].x - originScreen.x, tips[i].y - originScreen.y);
            float len = std::sqrt(d.x * d.x + d.y * d.y);
            if (len > 1e-3f) {
                d.x /= len;
                d.y /= len;
                ImVec2 perp(-d.y, d.x);
                const float head = 9.0f, halfW = 4.5f;
                ImVec2 base(tips[i].x - d.x * head, tips[i].y - d.y * head);
                dl->AddTriangleFilled(tips[i], ImVec2(base.x + perp.x * halfW, base.y + perp.y * halfW),
                                      ImVec2(base.x - perp.x * halfW, base.y - perp.y * halfW), col);
            }
        } else if (mode == Mode::Scale) {
            dl->AddRectFilled(ImVec2(tips[i].x - 4, tips[i].y - 4), ImVec2(tips[i].x + 4, tips[i].y + 4), col);
        } else {
            dl->AddCircle(tips[i], 6.0f, col, 12, active ? 3.0f : 2.0f);
        }
        dl->AddText(ImVec2(tips[i].x + 6, tips[i].y - 6), col, labels[i]);
    }
    dl->AddCircleFilled(originScreen, 3.5f, IM_COL32(230, 230, 230, 220));

    // Live numeric readout while dragging -- far easier to hit an exact
    // value with than watching the inspector fields tick past.
    if (st.dragging) {
        char buf[96];
        const Transform& t = e->local;
        if (mode == Mode::Translate) {
            std::snprintf(buf, sizeof(buf), "%.3f, %.3f, %.3f", t.position.x, t.position.y, t.position.z);
        } else if (mode == Mode::Scale) {
            std::snprintf(buf, sizeof(buf), "%.3f, %.3f, %.3f", t.scale.x, t.scale.y, t.scale.z);
        } else {
            Vec3 euler = t.rotation.toEuler();
            std::snprintf(buf, sizeof(buf), "%.1f, %.1f, %.1f deg", euler.x * PV_RAD2DEG,
                          euler.y * PV_RAD2DEG, euler.z * PV_RAD2DEG);
        }
        ImVec2 at(originScreen.x + 14, originScreen.y + 14);
        ImVec2 size = ImGui::CalcTextSize(buf);
        dl->AddRectFilled(ImVec2(at.x - 4, at.y - 3), ImVec2(at.x + size.x + 4, at.y + size.y + 3),
                          IM_COL32(20, 20, 24, 210), 3.0f);
        dl->AddText(at, IM_COL32(255, 255, 255, 255), buf);
    }

    return st.dragging || hovered >= 0;
}

} // namespace gizmo
} // namespace pv
