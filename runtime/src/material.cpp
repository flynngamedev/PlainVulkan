// material.cpp -- Pv::CreateMaterial / SetMaterialXxx. Values are stored
// for real (queryable, consistent), but -- as noted in draw3d.cpp -- the
// PlainVulkan reference has no "assign this material to that mesh"
// command, so DrawMesh currently always renders with the default blank
// (white) texture rather than a bound material's albedo. Tracked as a
// Tier 4 gap in runtime/README.md pending a material-binding API.
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

namespace pv {
namespace rt {

Value CreateMaterial() {
    MaterialRecord mat;
    return Value::MakeHandle(engine().materials.add(mat), "material");
}

Value SetMaterialAlbedo(const Value& material, const Value& texture) {
    if (auto* m = engine().materials.get(static_cast<uint64_t>(material.asInt()))) {
        m->albedoTexture = static_cast<uint64_t>(texture.asInt());
        m->materialSet = VK_NULL_HANDLE;
    }
    return Value();
}

Value SetMaterialNormal(const Value& material, const Value& texture) {
    if (auto* m = engine().materials.get(static_cast<uint64_t>(material.asInt()))) {
        m->normalTexture = static_cast<uint64_t>(texture.asInt());
        m->materialSet = VK_NULL_HANDLE;
    }
    return Value();
}

Value SetMaterialRoughness(const Value& material, const Value& value) {
    if (auto* m = engine().materials.get(static_cast<uint64_t>(material.asInt()))) {
        m->roughness = static_cast<float>(value.asFloat());
    }
    return Value();
}

Value SetMaterialMetallic(const Value& material, const Value& value) {
    if (auto* m = engine().materials.get(static_cast<uint64_t>(material.asInt()))) {
        m->metallic = static_cast<float>(value.asFloat());
    }
    return Value();
}

Value SetMaterialEmissive(const Value& material, const Value& texture, const Value& intensity) {
    if (auto* m = engine().materials.get(static_cast<uint64_t>(material.asInt()))) {
        m->emissiveTexture = static_cast<uint64_t>(texture.asInt());
        m->emissiveIntensity = static_cast<float>(intensity.asFloat());
    }
    return Value();
}

Value SetMaterialColor(const Value& material, const Value& r, const Value& g, const Value& b, const Value& a) {
    if (auto* m = engine().materials.get(static_cast<uint64_t>(material.asInt()))) {
        m->colorR = static_cast<float>(r.asFloat());
        m->colorG = static_cast<float>(g.asFloat());
        m->colorB = static_cast<float>(b.asFloat());
        m->colorA = static_cast<float>(a.asFloat());
    }
    return Value();
}

Value SetMaterialHeight(const Value& material, const Value& texture, const Value& scale) {
    if (auto* m = engine().materials.get(static_cast<uint64_t>(material.asInt()))) {
        m->heightTexture = static_cast<uint64_t>(texture.asInt());
        m->heightScale = static_cast<float>(scale.asFloat());
        m->materialSet = VK_NULL_HANDLE;
    }
    return Value();
}

} // namespace rt
} // namespace pv

