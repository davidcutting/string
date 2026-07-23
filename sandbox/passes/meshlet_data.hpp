#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <volk.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

namespace sandbox
{

// C++ mirrors of the pointer-loaded GPU meshlet structs in shaders/meshlet.slang. These are read via
// buffer_reference, so they use Slang's NATURAL layout (scalar packing: float3 12B/4-aligned,
// float4x4 64B/4-aligned, pointers 8-aligned). Every offset/size static_assert below is copied from
// the SPIR-V %<Struct>_natural OpMemberDecorate / ArrayStride ground truth (see meshlet.slang header
// for the slangc command). A drift here is silent GPU garbage or a GPUVM fault.

// meshopt build constants (grouped for tuning; brief 03 locked values).
inline constexpr uint32_t kMeshletMaxVertices = 64;
inline constexpr uint32_t kMeshletMaxTriangles = 124;
inline constexpr float kMeshletConeWeight = 0.5f;
inline constexpr uint32_t kMaxLods = 4;

// One meshlet (ArrayStride 48). Bounds + quantized cone dequantized to float here (int8 on disk).
struct GpuMeshlet
{
    uint32_t vertex_offset;    // 0
    uint32_t triangle_offset;  // 4
    uint32_t vertex_count;     // 8
    uint32_t triangle_count;   // 12
    glm::vec3 center;          // 16
    float radius;              // 28
    glm::vec3 cone_axis;       // 32
    float cone_cutoff;         // 44
};
static_assert(offsetof(GpuMeshlet, vertex_offset) == 0);
static_assert(offsetof(GpuMeshlet, triangle_offset) == 4);
static_assert(offsetof(GpuMeshlet, vertex_count) == 8);
static_assert(offsetof(GpuMeshlet, triangle_count) == 12);
static_assert(offsetof(GpuMeshlet, center) == 16);
static_assert(offsetof(GpuMeshlet, radius) == 28);
static_assert(offsetof(GpuMeshlet, cone_axis) == 32);
static_assert(offsetof(GpuMeshlet, cone_cutoff) == 44);
static_assert(sizeof(GpuMeshlet) == 48);

// Per-LOD meshlet range + simplification error (ArrayStride 16).
struct GpuMeshletLod
{
    uint32_t meshlet_offset;   // 0
    uint32_t meshlet_count;    // 4
    float error;               // 8
    uint32_t _pad;             // 12
};
static_assert(offsetof(GpuMeshletLod, meshlet_offset) == 0);
static_assert(offsetof(GpuMeshletLod, meshlet_count) == 4);
static_assert(offsetof(GpuMeshletLod, error) == 8);
static_assert(sizeof(GpuMeshletLod) == 16);

// Per-draw table (ArrayStride 208). `resident` is the streaming gate (host-visible write), `skinned`
// is reserved for Phase B (bounds inflation + cone-cull bypass).
struct GpuDrawInfo
{
    glm::mat4 model;               // 0
    glm::vec4 base_color;          // 64
    uint32_t base_slot;            // 80
    uint32_t normal_slot;          // 84
    uint32_t mr_slot;              // 88
    float metallic;                // 92
    float roughness;               // 96
    uint32_t occlusion_slot;       // 100
    uint32_t resident;             // 104
    uint32_t skinned;              // 108
    glm::vec3 center;              // 112
    float radius;                  // 124
    GpuMeshletLod lods[kMaxLods];  // 128
    uint32_t lod_count;            // 192
    uint32_t first_meshlet;        // 196
    uint32_t total_meshlets;       // 200
    // Signed heap delta (uint bits of int32): heap_vertex = global_index + vertex_offset, i.e.
    // a.vheap - range.vertex_offset, written by the streamer's residency callback. The meshlet
    // shaders MUST apply this — meshlet-vertices hold ORIGINAL global indices, but the vertex
    // heap is suballocated (the old path carried this in the indirect draw's vertexOffset).
    uint32_t vertex_offset;        // 204
    // Material rendering flags (brief 04): bit 0 = cutout (alpha-test), bit 1 = double-sided.
    uint32_t flags;                // 208
    float alpha_cutoff;            // 212 (MASK threshold; base-color alpha < this => clip())
};
// Brief 04 material flag bits (must match meshlet.slang kDrawFlag*).
inline constexpr uint32_t kDrawFlagCutout = 1u;
inline constexpr uint32_t kDrawFlagDoubleSided = 2u;
inline constexpr uint32_t kDrawFlagBlend = 4u;
static_assert(offsetof(GpuDrawInfo, model) == 0);
static_assert(offsetof(GpuDrawInfo, base_color) == 64);
static_assert(offsetof(GpuDrawInfo, base_slot) == 80);
static_assert(offsetof(GpuDrawInfo, occlusion_slot) == 100);
static_assert(offsetof(GpuDrawInfo, resident) == 104);
static_assert(offsetof(GpuDrawInfo, skinned) == 108);
static_assert(offsetof(GpuDrawInfo, center) == 112);
static_assert(offsetof(GpuDrawInfo, radius) == 124);
static_assert(offsetof(GpuDrawInfo, lods) == 128);
static_assert(offsetof(GpuDrawInfo, lod_count) == 192);
static_assert(offsetof(GpuDrawInfo, first_meshlet) == 196);
static_assert(offsetof(GpuDrawInfo, total_meshlets) == 200);
static_assert(offsetof(GpuDrawInfo, vertex_offset) == 204);
static_assert(offsetof(GpuDrawInfo, flags) == 208);
static_assert(offsetof(GpuDrawInfo, alpha_cutoff) == 212);
static_assert(sizeof(GpuDrawInfo) == 216);

// Scan block size — must match kScanBlock in meshlet.slang (the compaction two-pass block scan).
inline constexpr uint32_t kScanBlock = 256;

// GPU-written per-frame culling stats (read back for the UI overlay + log line). One uint per stage
// counter; the task/mesh shaders atomically increment. draws_per_lod is indexed by LOD level.
struct GpuMeshStats
{
    uint32_t meshlets_total;    // considered (resident draws x their LOD's meshlet count)
    uint32_t after_frustum;     // survived per-meshlet frustum cull
    uint32_t after_cone;        // survived backface-cone cull
    uint32_t after_hiz;         // survived HiZ occlusion (== drawn)
    uint32_t draws_per_lod[kMaxLods];
    uint32_t triangles;         // emitted triangles (approx: sum of drawn meshlet tri counts)
    uint32_t shadow_draws;      // brief 04 M4: draws surviving per-cascade shadow draw-cull (summed)
    uint32_t phase2_drawn;      // brief 04d: meshlets drawn in phase 2 (the disocclusion set). == stats[10]
    uint32_t _pad;
};
static_assert(sizeof(GpuMeshStats) == 48);
static_assert(offsetof(GpuMeshStats, shadow_draws) == 36);   // == stats[9] written by the cull compute
static_assert(offsetof(GpuMeshStats, phase2_drawn) == 40);   // == stats[10] written by the phase-2 task shader

// Brief 06 inspector: a per-draw snapshot the GeometryPass publishes each frame for the scene/draw
// inspector (read-only v1). Names come from the cooked scene if present, else "draw N".
struct InspectorDraw
{
    std::string name;
    uint32_t index = 0;
    int32_t material = -1;
    uint32_t meshlet_count = 0;
    uint32_t lod_count = 0;
    bool resident = false;
    glm::vec3 aabb_min{ 0.0f };
    glm::vec3 aabb_max{ 0.0f };
};

// Brief 06 inspector: a light snapshot (world position + range + colour).
struct InspectorLight
{
    glm::vec3 position{ 0.0f };
    float range = 0.0f;
    glm::vec3 color{ 1.0f };
    bool spot = false;
};

// Shared, CPU-side overlay state the GeometryPass fills each frame and the UI author reads (they are
// built separately in the RenderPlan, so a shared_ptr is the seam). Plain data, single-threaded
// (both run on the render thread), so no synchronization needed.
struct MeshOverlayStats
{
    GpuMeshStats stats{};       // last GPU-read culling counters
    bool hiz_enabled = false;
    bool crowd_enabled = false;
    int debug_view = 0;         // 0 none, 1 meshlet, 2 LOD, 3 occlusion-reject
    uint32_t total_meshlets = 0;
    uint32_t draw_count = 0;

    // Brief 06: camera + scene snapshot for the debug-line pass and the scene/draw inspector.
    glm::mat4 view_proj{ 1.0f };
    glm::vec3 camera_pos{ 0.0f };
    std::vector<InspectorDraw> draws;   // rebuilt when the draw set changes (load / crowd toggle)
    std::vector<InspectorLight> lights; // refreshed each frame (lights animate)
};

}  // namespace sandbox
