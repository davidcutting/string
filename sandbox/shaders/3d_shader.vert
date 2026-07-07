#version 450

// Camera is delivered as a precomputed MVP push constant (no descriptor). The material's
// base-color factor + texture slot ride in the same block for the fragment stage.
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 base_color;
    uint texture_slot;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec3 inNormal;   // unused for now (kept for lighting)

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
}
