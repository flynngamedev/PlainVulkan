#version 450
layout(location = 0) in vec2 vUV;
layout(set = 0, binding = 0) uniform sampler2D hdrColor;
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;
layout(set = 0, binding = 2) uniform sampler2D bloomTex;
layout(set = 0, binding = 3) uniform sampler2D historyTex;
layout(set = 0, binding = 4) uniform sampler2D colorLut;
layout(std140, set = 1, binding = 0) uniform SceneUBO {
    vec4 ambient;
    vec4 lightPosKind[4];
    vec4 lightColorIntensity[4];
    vec4 lightCountPad;
    vec4 camPos;
    vec4 jitterNearFar;
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invProj;
    mat4 lightVP[2];
} ubo;
layout(push_constant) uniform PostPC {
    vec4 params; // x=time, y=taaBlend, z=invW, w=invH
} pc;
layout(location = 0) out vec4 outColor;

float rand(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

float linearDepth(float d) {
    float n = ubo.jitterNearFar.z;
    float f = ubo.jitterNearFar.w;
    return (n * f) / max(f - d * (f - n), 1e-5);
}

vec3 reconstructView(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 view = ubo.invProj * clip;
    return view.xyz / view.w;
}

vec3 applyLut(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    float blue = c.b * 15.0;
    float slice0 = floor(blue);
    float slice1 = min(slice0 + 1.0, 15.0);
    float f = fract(blue);
    vec2 uv0 = vec2((slice0 + c.r) / 16.0, c.g);
    vec2 uv1 = vec2((slice1 + c.r) / 16.0, c.g);
    return mix(texture(colorLut, uv0).rgb, texture(colorLut, uv1).rgb, f);
}

void main() {
    vec2 texel = vec2(pc.params.z, pc.params.w);
    vec2 uv = vUV;
    vec4 hdr = texture(hdrColor, uv);
    float depth = texture(sceneDepth, uv).r;

    // SSAO from depth (skipped on sky / 2D where depth is 1)
    float ao = 1.0;
    if (depth < 0.999) {
        vec3 origin = reconstructView(uv, depth);
        float occ = 0.0;
        for (int i = 0; i < 8; ++i) {
            float a = float(i) * 2.399;
            vec2 off = vec2(cos(a), sin(a)) * (0.004 + 0.003 * float(i));
            vec2 suv = uv + off;
            float sd = texture(sceneDepth, suv).r;
            vec3 sampleV = reconstructView(suv, sd);
            float diff = origin.z - sampleV.z;
            occ += clamp(diff * 4.0, 0.0, 1.0) * (1.0 - smoothstep(0.4, 1.2, abs(diff)));
        }
        ao = 1.0 - occ / 8.0 * 0.65;
    }

    vec3 color = hdr.rgb * ao;

    // SSR: only on smooth surfaces (roughness packed in alpha)
    float roughness = hdr.a;
    if (depth < 0.999 && roughness < 0.28) {
        vec3 viewPos = reconstructView(uv, depth);
        vec3 V = normalize(-viewPos);
        float dx = linearDepth(texture(sceneDepth, uv + vec2(texel.x, 0)).r)
                 - linearDepth(depth);
        float dy = linearDepth(texture(sceneDepth, uv + vec2(0, texel.y)).r)
                 - linearDepth(depth);
        vec3 N = normalize(cross(vec3(texel.x, 0.0, dx), vec3(0.0, texel.y, dy)));
        vec3 R = reflect(-V, N);
        vec3 march = viewPos;
        vec3 hit = color;
        bool found = false;
        for (int i = 0; i < 16; ++i) {
            march += R * (0.15 + 0.05 * float(i));
            vec4 clip = ubo.proj * vec4(march, 1.0);
            vec2 muv = (clip.xy / clip.w) * 0.5 + 0.5;
            if (muv.x < 0.0 || muv.x > 1.0 || muv.y < 0.0 || muv.y > 1.0) break;
            float md = texture(sceneDepth, muv).r;
            vec3 mv = reconstructView(muv, md);
            if (march.z > mv.z + 0.02 && abs(march.z - mv.z) < 0.6) {
                hit = texture(hdrColor, muv).rgb;
                found = true;
                break;
            }
        }
        if (found) color = mix(color, hit, 0.45 * (1.0 - roughness));
    }

    color += texture(bloomTex, uv).rgb;

    // TAA: skip sky/UI (depth ~ 1)
    if (depth < 0.999) {
        vec3 hist = texture(historyTex, uv).rgb;
        color = mix(color, hist, pc.params.y);
    }

    color = aces(color);
    color = applyLut(color);
    float grain = (rand(uv * vec2(pc.params.z, pc.params.w) * 1000.0 + pc.params.x) - 0.5) * 0.035;
    color += grain;
    outColor = vec4(color, 1.0);
}
