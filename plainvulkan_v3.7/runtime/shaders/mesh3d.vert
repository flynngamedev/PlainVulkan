#version 450
layout(std140, set = 0, binding = 0) uniform SceneUBO {
    vec4 ambient;
    vec4 lightPosKind[4];
    vec4 lightColorIntensity[4];
    vec4 lightCountPad;   // x=count y=shadowsEnabled z=cascadeSplit
    vec4 camPos;          // xyz
    vec4 jitterNearFar;   // xy clip-space jitter, z=near, w=far
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invProj;
    mat4 lightVP[2];
} ubo;

layout(push_constant) uniform PushConsts {
    mat4 model;
    vec4 color;
    vec4 pbr; // roughness, metallic, emissiveIntensity, heightScale
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec2 fragUV;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out vec4 fragPbr;
layout(location = 4) out vec4 fragColor;

void main() {
    vec4 world = pc.model * vec4(inPos, 1.0);
    fragWorldPos = world.xyz;
    fragNormal = mat3(pc.model) * inNormal;
    fragUV = inUV;
    fragPbr = pc.pbr;
    fragColor = pc.color;
    vec4 clip = ubo.viewProj * world;
    clip.xy += ubo.jitterNearFar.xy * clip.w;
    gl_Position = clip;
}
