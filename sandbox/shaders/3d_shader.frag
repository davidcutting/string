#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Samples the draw's base-color texture out of the bindless table (set 0, binding 1) at the slot
// the vertex stage read from the per-draw storage buffer, tinted by the base-color factor.
layout(set = 0, binding = 1) uniform sampler2D textures[];

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in flat vec4 fragBaseColor;
layout(location = 2) in flat uint fragTextureSlot;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(textures[nonuniformEXT(fragTextureSlot)], fragTexCoord) * fragBaseColor;
}
