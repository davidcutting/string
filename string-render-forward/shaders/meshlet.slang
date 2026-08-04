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
    // Material rendering flags (brief 04): bit 0 = cutout (alpha-test), bit 1 = double-sided
    // (bypass cone cull, rasterizer CULL_NONE via a separate command list, flip normal to viewer).
    uint flags;
    float alpha_cutoff;    // MASK threshold, sampled base-color alpha < this => clip()
};
// Brief 04 material flag bits (shared C++/Slang meaning).
static const uint kDrawFlagCutout = 1u;
static const uint kDrawFlagDoubleSided = 2u;
static const uint kDrawFlagBlend = 4u;    // alpha-blend: excluded from opaque lists, drawn sorted in the transparency pass
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

// --- Brief 04c (resolution b): per-view scan-compacted per-draw command lists -------------------
// The per-draw dispatch fan-out (one indirect command per draw, empty for skipped draws — the 03b
// stable-slot scheme) is replaced by a two-phase cull that COMPACTS surviving draws: (1) a DRAW phase
// writes, at stable slot == draw index, each draw's surviving meshlet COUNT for this list + its
// selected LOD; (2) a COMPACTION phase runs an order-stable exclusive prefix scan over the survival
// predicate and a fill dispatch that writes, per SURVIVING draw at a dense contiguous slot in
// ascending draw order, one VkDrawMeshTasksIndirectCommandEXT (commands[]) + one {draw_index, lod}
// record (records[]), plus the surviving-draw count. One vkCmdDrawMeshTasksIndirectCountEXT then
// draws the whole list — draw count no longer drives dispatch cost, and command-index ordering pins
// coplanar depth-tie winners run-to-run (the 03b/RADV determinism constraint). The scan preserves
// input (draw) order, so submission order is deterministic (no atomics in the ordering path).

// Per-draw scratch the draw phase writes and the compaction consumes (stable slot == draw index).
struct UintBuffer { uint v[]; };

// Scan block size — one workgroup scans this many per-draw survival predicates in pass A.
static const uint kScanBlock = 256;

// --- Brief 04d: persistent per-meshlet visibility bitfield -----------------------------------------
// One bit per GLOBAL meshlet id (draw's lods[lod].meshlet_offset + local). Sized to total meshlet
// capacity, zero-initialized on creation (a cleared bit => the meshlet takes phase 2 for one frame:
// warmup = fallback = streaming/LOD-change path, all correct). Persists across frames (NOT ringed).
// Phase 1 reads a bit (visible last frame). Phase 2 sets it when the meshlet is HiZ-visible now, or
// clears it when HiZ-hidden. Set/clear are atomic (distinct meshlets may share a 32-bit word, and
// many task workgroups touch the same word in the same frame).
struct VisBitfield { uint words[]; };

bool vis_get(VisBitfield *bf, uint id)
{
    return (bf.words[id >> 5u] & (1u << (id & 31u))) != 0u;
}
void vis_set(VisBitfield *bf, uint id)
{
    uint prev;
    InterlockedOr(bf.words[id >> 5u], (1u << (id & 31u)), prev);
}
void vis_clear(VisBitfield *bf, uint id)
{
    uint prev;
    InterlockedAnd(bf.words[id >> 5u], ~(1u << (id & 31u)), prev);
}
