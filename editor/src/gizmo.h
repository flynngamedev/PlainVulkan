// gizmo.h -- viewport mouse picking and the transform manipulator.
#pragma once

#include "pv/pv_math.h"
#include "pv/pv_scene.h"

#include <cstdint>

namespace pv {
namespace gizmo {

struct Ray {
    Vec3 origin;
    Vec3 direction; // normalized
};

enum class Mode { Translate, Rotate, Scale };

struct State {
    bool dragging = false;
    int axis = 0; // 0=X, 1=Y, 2=Z
    Transform startTransform;
    float dragStartParam = 0.0f;
    // Local space manipulates along the entity's own axes; world space along
    // the global ones. Local is the useful default for a rotated object.
    bool localSpace = false;
    // Snap increment in world units (0 = free). Applied to the drag delta
    // rather than the absolute value, so snapping is relative to where the
    // object started rather than to the world grid.
    float snap = 0.0f;
};

// Builds a world-space ray through a pixel. Works for orthographic cameras
// too, because it unprojects two depths rather than deriving a direction
// from the field of view.
Ray screenRay(const Mat4& view, const Mat4& proj, float mouseX, float mouseY, float screenW, float screenH);

// Nearest selectable entity along the ray, or 0. Tests bounding spheres --
// see the note in gizmo.cpp for why not exact geometry.
uint64_t pickEntity(Scene& s, const Ray& ray);

// Draws the manipulator and applies any drag to the entity's local
// transform. Returns true when the gizmo is hovered or being dragged, which
// the caller uses to suppress click-to-select underneath it.
bool manipulate(Scene& s, uint64_t entityId, Mode mode, State& st, const Mat4& view, const Mat4& proj,
                float screenW, float screenH, bool mouseDown, bool mousePressed, float mouseX, float mouseY);

} // namespace gizmo
} // namespace pv
