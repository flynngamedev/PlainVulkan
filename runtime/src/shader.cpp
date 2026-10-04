// shader.cpp -- Pv::LoadShader / SetUniformXxx / ... .
//
// Tier 4 (see runtime/README.md): handles are tracked and every call is
// safe to make, but custom user shaders don't yet replace the built-in
// 2D/3D pipelines -- doing that properly means compiling arbitrary GLSL
// to SPIR-V at build time (or embedding a runtime compiler like shaderc)
// and building a pipeline-variant cache keyed by shader+vertex-format,
// which is a real subsystem of its own. Rendering always uses the built-in
// pipelines from vk_core.cpp regardless of Pv::SetShader.
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

namespace pv {
namespace rt {

Value LoadShader(const Value& vertPath, const Value& fragPath) {
    std::string label = vertPath.asString() + " + " + fragPath.asString();
    logLine("WARNING", "Pv::LoadShader('" + label +
                            "'): tracked but custom shaders don't replace the built-in pipeline yet (Tier 4, "
                            "see runtime/README.md)");
    return Value::MakeHandle(engine().shaderLabels.add(label), "shader");
}

Value UnloadShader(const Value& shader) {
    engine().shaderLabels.remove(static_cast<uint64_t>(shader.asInt()));
    return Value();
}

Value SetShader(const Value& shader) {
    (void)shader;
    return Value();
}

Value ResetShader() { return Value(); }

Value SetUniformInt(const Value& shader, const Value& name, const Value& value) {
    (void)shader;
    (void)name;
    (void)value;
    return Value();
}
Value SetUniformFloat(const Value& shader, const Value& name, const Value& value) {
    (void)shader;
    (void)name;
    (void)value;
    return Value();
}
Value SetUniformVec2(const Value& shader, const Value& name, const Value& x, const Value& y) {
    (void)shader;
    (void)name;
    (void)x;
    (void)y;
    return Value();
}
Value SetUniformVec3(const Value& shader, const Value& name, const Value& x, const Value& y, const Value& z) {
    (void)shader;
    (void)name;
    (void)x;
    (void)y;
    (void)z;
    return Value();
}
Value SetUniformVec4(const Value& shader, const Value& name, const Value& x, const Value& y, const Value& z,
                      const Value& w) {
    (void)shader;
    (void)name;
    (void)x;
    (void)y;
    (void)z;
    (void)w;
    return Value();
}
Value SetUniformMat4(const Value& shader, const Value& name, const Value& matrix) {
    (void)shader;
    (void)name;
    (void)matrix;
    return Value();
}
Value SetUniformTexture(const Value& shader, const Value& name, const Value& texture, const Value& slot) {
    (void)shader;
    (void)name;
    (void)texture;
    (void)slot;
    return Value();
}

} // namespace rt
} // namespace pv
