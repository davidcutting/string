#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require

// Depth-only shadow pass: renders scene depth from the sun's orthographic view. Reuses the main
// pass's per-draw data (for the model transform) and programmable vertex pulling; no fragment
// shader (depth only). One vkCmdDrawIndexedIndirect over an all-visible command buffer draws every
// mesh, so gl_InstanceIndex -> DrawData via firstInstance exactly like the main pass.
struct DrawData {
    mat4 model;
    vec4 base_color;
    uint base_slot;
    uint normal_slot;
    uint mr_slot;
    float metallic;
    float roughness;
};

layout(set = 0, binding = 0, std430) readonly buffer DrawDataBuffer {
    DrawData draws[];
} draw_buffers[];

struct Vertex {
    vec3 pos;
    vec3 color;
    vec2 texCoord;
    vec3 normal;
};

layout(buffer_reference, scalar) readonly buffer VertexBuffer {
    Vertex verts[];
};

layout(push_constant) uniform Push {
    mat4 light_view_proj;
    VertexBuffer vertices;
    uint drawdata_slot;
} pc;

void main() {
    DrawData d = draw_buffers[pc.drawdata_slot].draws[gl_InstanceIndex];
    Vertex v = pc.vertices.verts[gl_VertexIndex];
    gl_Position = pc.light_view_proj * d.model * vec4(v.pos, 1.0);
}
