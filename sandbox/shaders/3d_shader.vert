#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require

// GPU-driven: per-draw data lives in a storage buffer bound into the bindless table (set 0,
// binding 0). One vkCmdDrawIndexedIndirect issues every draw; each indirect command sets
// firstInstance to its draw index, so gl_InstanceIndex selects this draw's record.
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

// Programmable vertex pulling: vertices live in a device-address SSBO. scalar layout matches the
// tightly-packed C++ String::Vertex (vec3 pos / vec3 color / vec2 texCoord / vec3 normal, stride
// 44) exactly — do NOT use std430 here.
struct Vertex {
    vec3 pos;
    vec3 color;
    vec2 texCoord;
    vec3 normal;
};

layout(buffer_reference, scalar) readonly buffer VertexBuffer {
    Vertex verts[];
};

// Must match GeometryPush in geometry_pass.hpp (mat4, address, uint, pad, then the fragment-stage
// lighting block). The vertex stage only reads the first fields.
layout(push_constant) uniform Push {
    mat4 view_proj;
    VertexBuffer vertices;
    uint drawdata_slot;
    uint _pad0;
    vec3 camera_pos;      float _pad1;
    vec3 sun_dir;         float sun_intensity;
    vec3 sun_color;       float _pad2;
    vec3 ambient_sky;     float _pad3;
    vec3 ambient_ground;  float _pad4;
} pc;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec3 fragWorldPos;
layout(location = 2) out vec3 fragWorldNormal;
layout(location = 3) out flat vec4 fragBaseColor;
layout(location = 4) out flat uint fragBaseSlot;
layout(location = 5) out flat uint fragNormalSlot;
layout(location = 6) out flat uint fragMrSlot;
layout(location = 7) out flat vec2 fragMetalRough;

void main() {
    DrawData d = draw_buffers[pc.drawdata_slot].draws[gl_InstanceIndex];
    Vertex v = pc.vertices.verts[gl_VertexIndex];

    vec4 world = d.model * vec4(v.pos, 1.0);
    gl_Position = pc.view_proj * world;

    fragTexCoord = v.texCoord;
    fragWorldPos = world.xyz;
    // mat3(model) is correct for rigid / uniform-scale transforms (Sponza); non-uniform scale would
    // want the inverse-transpose normal matrix.
    fragWorldNormal = mat3(d.model) * v.normal;
    fragBaseColor = d.base_color;
    fragBaseSlot = d.base_slot;
    fragNormalSlot = d.normal_slot;
    fragMrSlot = d.mr_slot;
    fragMetalRough = vec2(d.metallic, d.roughness);
}
