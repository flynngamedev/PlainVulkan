// camera.cpp -- Pv::SetCamera / LookAt / SetFOV / ... . Just updates
// Engine state; the actual view/projection matrices are built lazily each
// draw call by buildViewMatrix()/buildProjectionMatrix() in vk_core.cpp.
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

namespace pv {
namespace rt {

Value SetCamera(const Value& x, const Value& y, const Value& z) {
    auto& e = engine();
    e.camPos = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()), static_cast<float>(z.asFloat()));
    return Value();
}

Value LookAt(const Value& x, const Value& y, const Value& z) {
    auto& e = engine();
    e.camTarget = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()), static_cast<float>(z.asFloat()));
    e.camHasTarget = true;
    return Value();
}

Value SetFOV(const Value& degrees) {
    engine().fovDegrees = static_cast<float>(degrees.asFloat());
    return Value();
}

Value SetNearFar(const Value& near_, const Value& far_) {
    engine().nearZ = static_cast<float>(near_.asFloat());
    engine().farZ = static_cast<float>(far_.asFloat());
    return Value();
}

Value SetOrthographic(const Value& enabled) {
    engine().orthographic = enabled.truthy();
    return Value();
}

Value MoveCamera(const Value& x, const Value& y, const Value& z) {
    auto& e = engine();
    Vec3 delta(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()), static_cast<float>(z.asFloat()));
    e.camPos = e.camPos + delta;
    if (e.camHasTarget) e.camTarget = e.camTarget + delta;
    return Value();
}

Value RotateCamera(const Value& pitch, const Value& yaw, const Value& roll) {
    auto& e = engine();
    e.camPitch += static_cast<float>(pitch.asFloat());
    e.camYaw += static_cast<float>(yaw.asFloat());
    e.camRoll += static_cast<float>(roll.asFloat());
    e.camHasTarget = false; // rotation-driven look direction supersedes an explicit LookAt target
    return Value();
}

} // namespace rt
} // namespace pv
