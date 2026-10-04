#version 450
layout(location = 0) in vec2 vUV;
layout(set = 0, binding = 0) uniform sampler2D hdrColor;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(hdrColor, 0));
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    const float k0 = 0.294117;
    const float k1 = 0.352941;
    const float k2 = 0.176470;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec3 s = texture(hdrColor, vUV + vec2(float(x), float(y)) * texel * 1.6).rgb;
            vec3 bright = max(s - vec3(1.0), vec3(0.0));
            int ax = x < 0 ? -x : x;
            int ay = y < 0 ? -y : y;
            float wx = ax == 0 ? k0 : (ax == 1 ? k1 : k2);
            float wy = ay == 0 ? k0 : (ay == 1 ? k1 : k2);
            float w = wx * wy;
            acc += bright * w;
            wsum += w;
        }
    }
    outColor = vec4(acc / max(wsum, 0.0001), 1.0);
}
