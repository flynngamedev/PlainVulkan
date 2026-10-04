#version 450
// Particle quads arrive already expanded to world-space corners on the CPU
// (see particles.cpp), so this stage is a plain view-projection transform.
// Building the quads CPU-side rather than in a geometry/vertex shader keeps
// the pipeline free of geometry-shader support requirements -- which some
// mobile and older integrated GPUs genuinely lack -- and lets the CPU sort
// them back-to-front for correct alpha blending in the same pass.
layout(push_constant) uniform PushConsts {
    mat4 viewProj;
    vec4 params;   // x = soft-particle depth fade distance (0 = off), yzw reserved
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;

void main() {
    gl_Position = pc.viewProj * vec4(inPos, 1.0);
    fragColor = inColor;
    fragUV = inUV;
}
