// Shared meshlet data model for the task/mesh geometry pipeline (brief 03). All these structs are
// read via buffer_reference (pointer), so they use Slang's NATURAL layout: float3 is 12B/4-aligned,
// float4x4 is 64B/4-aligned, pointers 8-aligned. The C++ mirrors (meshlet_data.hpp) carry
// static_asserts against the offsets emitted here; verify with:
//   slangc meshlet.slang -target spirv-asm -entry mesh_main -stage mesh -emit-spirv-directly \
//     | grep 'OpMemberDecorate %<Struct>_natural'
// NEVER trust hand-computed std430 rules for these.

// meshopt build limits (must match meshlet_data.hpp). Mesh-shader output-array sizes reference these.
static const uint kMeshletMaxVertices = 64;
static const uint kMeshletMaxTriangles = 124;

// Matches String::Vertex (tight 48B scalar layout); tangent is packed 10:10:10:2 read as a uint.
struct Vertex
{
    float3 pos;
    float3 color;
    float2 texCoord;
    float3 normal;
    uint tangent;
};
struct VertexBuffer { Vertex verts[]; };

// One meshlet: a range into the meshlet-vertex remap + the packed local triangle indices, plus
// the meshopt-computed bounding sphere and quantized cone (for frustum + backface-cone culling).
struct Meshlet
{
    uint vertex_offset;    // start in the meshlet-vertex remap heap (global vertex indices)
    uint triangle_offset;  // start in the packed-triangle heap (bytes/4 = uint index)
    uint vertex_count;     // <= 64
    uint triangle_count;   // <= 124
    float3 center;         // bounding-sphere center, model space
    float radius;          // bounding-sphere radius, model space
    float3 cone_axis;      // normalized cone axis (dequantized from int8 on the CPU to float here)
    float cone_cutoff;     // cos of the cone half-angle; backface if dot(axis, view) >= cutoff
};
struct MeshletBuffer { Meshlet meshlets[]; };

// meshlet-vertices: uint remap into the shared vertex heap (global indices — keeps the existing
// HeapSuballocator/global-index scheme intact). meshlet-triangles: packed 8-bit local indices,
// u32-packed (3 bytes per triangle, read via byte extraction in the mesh shader).
struct MeshletVertexBuffer { uint indices[]; };
struct MeshletTriangleBuffer { uint packed[]; };

// Per source draw, one per LOD level: the meshlet range for that LOD and its simplification error.
struct MeshletLod
{
    uint meshlet_offset;   // first meshlet of this LOD in the global meshlet buffer
    uint meshlet_count;
    float error;           // meshopt simplification error, model-space units (0 for LOD0)
    uint _pad;
};

// Per-draw table: the transform, material slots, per-LOD meshlet ranges, a resident flag (the
// streaming gate), and a skinned flag reserved for Phase B (skinned draws inflate bounds + bypass
// cone cull). 4 LOD levels max.
struct DrawInfo
{
    float4x4 model;
    float4 base_color;
    uint base_slot;
    uint normal_slot;
    uint mr_slot;
    float metallic;
    float roughness;
    uint occlusion_slot;
    uint resident;         // 0 = not streamed in (skip), 1 = resident (draw). Streaming gate.
    uint skinned;          // reserved (Phase B): 1 = bypass cone cull, bounds pre-inflated
    float3 center;         // draw bounding-sphere center, model space (for LOD screen-error + cull)
    float radius;          // draw bounding-sphere radius, model space
    MeshletLod lods[4];
    uint lod_count;
    uint first_meshlet;    // global meshlet id of this draw's LOD0 meshlet 0 (HiZ bitfield base)
    uint total_meshlets;   // meshlets across all LODs (for the global visibility bitfield stride)
    // Signed heap delta (int32 as uint bits): meshlet-vertices hold ORIGINAL global vertex
    // indices; the vertex heap is suballocated. heap_vertex = uint(int(global) + int(this)).
    uint vertex_offset;
};
struct DrawInfoBuffer { DrawInfo draws[]; };

// --- Brief 03b: GPU-driven draw generation --------------------------------------------------------
// The draw-cull compute (meshlet_draw_cull.slang) writes one entry per surviving draw into two
// parallel arrays + a count, then the CPU issues a single vkCmdDrawMeshTasksIndirectCountEXT. The
// task shader reads its record via the SV_DrawIndex builtin (draw index into these arrays).

// VkDrawMeshTasksIndirectCommandEXT: {groupCountX, groupCountY, groupCountZ}. 12B, matches the Vulkan
// struct exactly (indirect buffer read by the driver, NOT via buffer_reference natural layout).
struct MeshTaskCommand { uint x; uint y; uint z; };
struct MeshTaskCommandBuffer { MeshTaskCommand cmds[]; };

// Parallel to commands[]: which draw + which LOD each dispatched workgroup-set belongs to.
struct DrawRecord { uint draw_index; uint lod; };
struct DrawRecordBuffer { DrawRecord records[]; };

struct CountBuffer { uint count; };
