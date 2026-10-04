// lighting.cpp -- Pv::SetAmbient / AddPointLight / ... . These write
// directly into the Engine state that updateLightsUbo() (vk_core.cpp)
// uploads every frame, so ambient + up to 4 point/directional lights
// genuinely affect DrawMesh's shading. Spot lights are accepted and
// stored but shaded as point lights (no cone falloff) -- see
// runtime/README.md. Shadows/fog are tracked flags without a rendering
// effect yet (Tier 4).
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

namespace pv {
namespace rt {

Value SetAmbient(const Value& r, const Value& g, const Value& b, const Value& intensity) {
    auto& e = engine();
    e.ambientR = static_cast<float>(r.asFloat());
    e.ambientG = static_cast<float>(g.asFloat());
    e.ambientB = static_cast<float>(b.asFloat());
    e.ambientIntensity = static_cast<float>(intensity.asFloat());
    return Value();
}

Value AddPointLight(const Value& x, const Value& y, const Value& z, const Value& r, const Value& g, const Value& b,
                     const Value& intensity, const Value& radius) {
    LightRecord light;
    light.kind = LightKind::Point;
    light.position = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()), static_cast<float>(z.asFloat()));
    light.r = static_cast<float>(r.asFloat());
    light.g = static_cast<float>(g.asFloat());
    light.b = static_cast<float>(b.asFloat());
    light.intensity = static_cast<float>(intensity.asFloat());
    light.radius = static_cast<float>(radius.asFloat());
    uint64_t id = engine().lights.add(light);
    return Value::MakeHandle(id, "light");
}

Value AddDirectionalLight(const Value& dx, const Value& dy, const Value& dz, const Value& r, const Value& g,
                           const Value& b, const Value& intensity) {
    LightRecord light;
    light.kind = LightKind::Directional;
    light.direction = Vec3(static_cast<float>(dx.asFloat()), static_cast<float>(dy.asFloat()), static_cast<float>(dz.asFloat()));
    light.r = static_cast<float>(r.asFloat());
    light.g = static_cast<float>(g.asFloat());
    light.b = static_cast<float>(b.asFloat());
    light.intensity = static_cast<float>(intensity.asFloat());
    uint64_t id = engine().lights.add(light);
    return Value::MakeHandle(id, "light");
}

Value AddSpotLight(const Value& x, const Value& y, const Value& z, const Value& dx, const Value& dy,
                    const Value& dz, const Value& angle, const Value& intensity) {
    LightRecord light;
    light.kind = LightKind::Spot; // shaded as a point light for now -- see file header
    light.position = Vec3(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()), static_cast<float>(z.asFloat()));
    light.direction = Vec3(static_cast<float>(dx.asFloat()), static_cast<float>(dy.asFloat()), static_cast<float>(dz.asFloat()));
    light.angle = static_cast<float>(angle.asFloat());
    light.intensity = static_cast<float>(intensity.asFloat());
    light.r = light.g = light.b = 1.0f;
    uint64_t id = engine().lights.add(light);
    return Value::MakeHandle(id, "light");
}

Value RemoveLight(const Value& light) {
    engine().lights.remove(static_cast<uint64_t>(light.asInt()));
    return Value();
}

Value SetShadows(const Value& enabled) {
    engine().shadowsEnabled = enabled.truthy();
    return Value();
}

Value SetShadowResolution(const Value& resolution) {
    engine().shadowResolution = static_cast<int>(resolution.asInt());
    return Value();
}

Value SetFog(const Value& r, const Value& g, const Value& b, const Value& near_, const Value& far_) {
    auto& e = engine();
    e.fogR = static_cast<float>(r.asFloat());
    e.fogG = static_cast<float>(g.asFloat());
    e.fogB = static_cast<float>(b.asFloat());
    e.fogNear = static_cast<float>(near_.asFloat());
    e.fogFar = static_cast<float>(far_.asFloat());
    e.fogEnabled = true;
    logLine("WARNING", "Pv::SetFog: value is tracked but the built-in shader doesn't apply fog yet (Tier 4 gap, "
                        "see runtime/README.md)");
    return Value();
}

} // namespace rt
} // namespace pv
