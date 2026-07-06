#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Samples the model's texture out of the bindless table (set 0, binding 1) at the slot
// supplied in the push constant.
layout(set = 0, binding = 1) uniform sampler2D textures[];

layout(push_constant) uniform Push {
    mat4 mvp;
    uint texture_slot;
} pc;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(textures[nonuniformEXT(pc.texture_slot)], fragTexCoord);
}
