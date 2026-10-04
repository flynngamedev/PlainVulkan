#version 450
layout(set = 0, binding = 0) uniform sampler2D particleTex;

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 texel = texture(particleTex, fragUV);
    vec4 c = texel * fragColor;

    // Discarding fully transparent fragments matters more here than in the
    // sprite path: an emitter covers a large screen area with mostly-empty
    // quads, and for the additive pipeline (which has depth-write disabled
    // but still runs the depth test) it avoids paying for blend on pixels
    // that contribute nothing.
    if (c.a < 0.004) discard;

    outColor = c;
}
