#pragma once

#include <cstddef>
#include <cstdint>

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
};
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
static_assert(sizeof(GpuDrawInfo) == 208);

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
    uint32_t _pad[3];
};
static_assert(sizeof(GpuMeshStats) == 48);

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
};

}  // namespace sandbox
