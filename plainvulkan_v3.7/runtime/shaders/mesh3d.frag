#version 450
layout(std140, set = 0, binding = 0) uniform SceneUBO {
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

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadowMap;
layout(set = 1, binding = 0) uniform sampler2D albedoTex;
layout(set = 1, binding = 1) uniform sampler2D normalTex;
layout(set = 1, binding = 2) uniform sampler2D heightTex;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in vec4 fragPbr;
layout(location = 4) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265;

vec3 irradianceIBL(vec3 N) {
    float hemi = N.y * 0.5 + 0.5;
    vec3 ground = vec3(0.04, 0.035, 0.03);
    vec3 sky = vec3(0.38, 0.45, 0.58);
    return mix(ground, sky, hemi);
}

vec3 specularIBL(vec3 R, float roughness) {
    float hemi = clamp(R.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 horizon = vec3(0.18, 0.16, 0.14);
    vec3 zenith = vec3(0.72, 0.80, 0.95);
    return mix(horizon, zenith, hemi) * (1.0 - roughness * 0.85);
}

float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float geometrySchlick(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec2 parallaxUV(vec2 uv, vec3 viewTS, float scale) {
    if (scale < 0.0001) return uv;
    const int steps = 8;
    float layer = 1.0 / float(steps);
    vec2 delta = viewTS.xy * (scale / max(viewTS.z, 0.1)) * layer;
    vec2 p = uv;
    float height = 1.0 - texture(heightTex, p).r;
    float depth = 0.0;
    for (int i = 0; i < steps; ++i) {
        if (depth >= height) break;
        p -= delta;
        height = 1.0 - texture(heightTex, p).r;
        depth += layer;
    }
    return p;
}

float shadowAt(vec3 worldPos, vec3 N, vec3 L) {
    if (ubo.lightCountPad.y < 0.5) return 1.0;
    float dist = length(worldPos - ubo.camPos.xyz);
    int cascade = dist > ubo.lightCountPad.z ? 1 : 0;
    vec4 ls = ubo.lightVP[cascade] * vec4(worldPos, 1.0);
    vec3 ndc = ls.xyz / max(ls.w, 1e-5);
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    float bias = max(0.002 * (1.0 - dot(N, L)), 0.0005);
    float d = ndc.z - bias;
    return texture(shadowMap, vec4(uv, float(cascade), d));
}

void main() {
    vec3 N = normalize(fragNormal);
    vec3 V = normalize(ubo.camPos.xyz - fragWorldPos);
    // Tangent-space from screen-space derivatives so meshes need no tangents.
    vec3 dp1 = dFdx(fragWorldPos);
    vec3 dp2 = dFdy(fragWorldPos);
    vec2 du1 = dFdx(fragUV);
    vec2 du2 = dFdy(fragUV);
    vec3 T = normalize(dp1 * du2.y - dp2 * du1.y);
    vec3 B = normalize(cross(N, T));
    mat3 TBN = mat3(T, B, N);
    vec3 viewTS = normalize(transpose(TBN) * V);

    vec2 uv = parallaxUV(fragUV, viewTS, fragPbr.w);
    vec3 albedo = texture(albedoTex, uv).rgb * fragColor.rgb;
    vec3 nSamp = texture(normalTex, uv).xyz * 2.0 - 1.0;
    N = normalize(TBN * nSamp);

    float roughness = clamp(fragPbr.x, 0.04, 1.0);
    float metallic = clamp(fragPbr.y, 0.0, 1.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);
    int count = int(ubo.lightCountPad.x);
    for (int i = 0; i < count && i < 4; ++i) {
        vec3 L;
        float atten = 1.0;
        if (ubo.lightPosKind[i].w >= 0.5) {
            L = normalize(-ubo.lightPosKind[i].xyz);
        } else {
            vec3 toL = ubo.lightPosKind[i].xyz - fragWorldPos;
            float dist = length(toL);
            L = toL / max(dist, 1e-4);
            atten = 1.0 / (1.0 + dist * dist * 0.05);
        }
        vec3 H = normalize(V + L);
        float NdotL = max(dot(N, L), 0.0);
        float NdotV = max(dot(N, V), 0.0);
        float NdotH = max(dot(N, H), 0.0);
        float D = distributionGGX(NdotH, roughness);
        float G = geometrySchlick(NdotV, roughness) * geometrySchlick(NdotL, roughness);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
        vec3 spec = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);
        vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
        float sh = (i == 0 && ubo.lightPosKind[i].w >= 0.5) ? shadowAt(fragWorldPos, N, L) : 1.0;
        vec3 radiance = ubo.lightColorIntensity[i].rgb * ubo.lightColorIntensity[i].a * atten * sh;
        Lo += (kD * albedo / PI + spec) * radiance * NdotL;
    }

    vec3 F = fresnelSchlick(max(dot(N, V), 0.0), F0);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 ambientCol = irradianceIBL(N) * albedo * kD * ubo.ambient.a
                    + specularIBL(reflect(-V, N), roughness) * F;
    ambientCol *= mix(vec3(1.0), ubo.ambient.rgb, 0.35);
    vec3 color = ambientCol + Lo + albedo * fragPbr.z;
    outColor = vec4(color, roughness);
}
