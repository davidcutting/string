#version 450
#extension GL_EXT_nonuniform_qualifier : require

// GPU-driven: per-draw data lives in a storage buffer bound into the bindless table (set 0,
// binding 0). One vkCmdDrawIndexedIndirect issues every draw; each indirect command sets
// firstInstance to its draw index, so gl_InstanceIndex selects this draw's record. The camera's
// view-projection and the draw-data buffer's bindless slot ride in the push constant.
struct DrawData {
    mat4 model;
    vec4 base_color;
    uint texture_slot;
};

layout(set = 0, binding = 0, std430) readonly buffer DrawDataBuffer {
    DrawData draws[];
} draw_buffers[];

layout(push_constant) uniform Push {
    mat4 view_proj;
    uint drawdata_slot;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec3 inNormal;   // unused for now (kept for lighting)

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out flat vec4 fragBaseColor;
layout(location = 2) out flat uint fragTextureSlot;

void main() {
    DrawData d = draw_buffers[pc.drawdata_slot].draws[gl_InstanceIndex];
    gl_Position = pc.view_proj * d.model * vec4(inPosition, 1.0);
    fragTexCoord = inTexCoord;
    fragBaseColor = d.base_color;
    fragTextureSlot = d.texture_slot;
}
