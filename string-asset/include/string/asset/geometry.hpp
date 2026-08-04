#pragma once

#include <cstddef>
#include <cstdint>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

namespace string::asset
{

// ============================================================================
// Baked geometry representation
// ============================================================================
//
// The meshlet form the cook produces and every renderer consumes. This lives in string-asset (not
// in a renderer) deliberately: meshlet + LOD chain + bounds is the standard representation for a
// GPU-driven renderer, and keeping ONE deterministic bake is a hard requirement — procgen and the
// server both depend on identical inputs producing identical output.
//
// What is NOT here: GpuDrawInfo, the per-frame stats/inspector structs, and anything holding a
// material or transform binding. Those are renderer-owned — a draw's material/transform binding is
// the renderer's business, its geometry is the asset's. The cooked format follows that same line
// (CookedDraw is geometry-only; the renderer fills material/transform at load).
//
// These are read on the GPU via buffer_reference, so they use Slang's NATURAL layout (scalar
// packing: float3 12B/4-aligned). Every offset/size static_assert below is copied from the SPIR-V
// %<Struct>_natural OpMemberDecorate / ArrayStride ground truth (see the renderer's meshlet.slang
// header for the slangc command). A drift here is silent GPU garbage or a GPUVM fault.

// meshopt build constants (grouped for tuning; brief 03 locked values). Baked into the cooked
// header and cross-checked on load — changing one invalidates every cooked file.
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

}  // namespace string::asset
