#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require

// GPU-driven: per-draw data lives in a storage buffer bound into the bindless table (set 0,
// binding 0). One vkCmdDrawIndexedIndirect issues every draw; each indirect command sets
// firstInstance to its draw index, so gl_InstanceIndex selects this draw's record. The camera's
// view-projection, the vertex buffer's device address, and the draw-data buffer's bindless slot
// ride in the push constant.
struct DrawData {
    mat4 model;
    vec4 base_color;
    uint texture_slot;
};

layout(set = 0, binding = 0, std430) readonly buffer DrawDataBuffer {
    DrawData draws[];
} draw_buffers[];

// Programmable vertex pulling: vertices live in a device-address SSBO instead of a fixed-function
// vertex binding. scalar layout makes this struct match the tightly-packed C++ String::Vertex
// (vec3 pos / vec3 color / vec2 texCoord / vec3 normal, stride 44) exactly — do NOT use std430
// here, its vec3 padding would misalign every vertex. gl_VertexIndex indexes it directly; indices
// are still fetched fixed-function by the indexed indirect draw.
struct Vertex {
    vec3 pos;
    vec3 color;
    vec2 texCoord;
    vec3 normal;   // unused for now (kept for lighting)
};

layout(buffer_reference, scalar) readonly buffer VertexBuffer {
    Vertex verts[];
};

layout(push_constant) uniform Push {
    mat4 view_proj;
    VertexBuffer vertices;
    uint drawdata_slot;
} pc;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out flat vec4 fragBaseColor;
layout(location = 2) out flat uint fragTextureSlot;

void main() {
    DrawData d = draw_buffers[pc.drawdata_slot].draws[gl_InstanceIndex];
    Vertex v = pc.vertices.verts[gl_VertexIndex];
    gl_Position = pc.view_proj * d.model * vec4(v.pos, 1.0);
    fragTexCoord = v.texCoord;
    fragBaseColor = d.base_color;
    fragTextureSlot = d.texture_slot;
}
