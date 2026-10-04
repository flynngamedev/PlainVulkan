#version 450
// Shared by pipeline2D (flat shapes) and pipelineSprite (textured quads) --
// both pipelines use this exact vertex stage; only the fragment shader
// differs (prim2d.frag ignores UV, sprite.frag samples with it).
layout(push_constant) uniform PushConsts {
    vec2 screenSize;
} pc;

layout(location = 0) in vec2 inPos;   // pixel coordinates, origin top-left
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;

void main() {
    // Vulkan's NDC is already X-right/Y-down in [-1,1], matching a
    // top-left-origin pixel-space convention directly -- no Y flip needed.
    vec2 ndc = (inPos / pc.screenSize) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    fragColor = inColor;
    fragUV = inUV;
}
