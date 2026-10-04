#version 450
layout(push_constant) uniform Push {
    mat4 lightVP;
    mat4 model;
} pc;
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
void main() {
    gl_Position = pc.lightVP * pc.model * vec4(inPos, 1.0);
}
