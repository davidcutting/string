#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <string/render/gltf_types.hpp>
#include <string/render/meshlet_data.hpp>

namespace string::render
{

// The meshletized form of a whole model (brief 03, milestone 1). Built at load from the flattened
// glTF geometry with meshoptimizer: each draw is simplified into up to kMaxLods discrete LOD levels,
// each level meshletized (64 verts / 124 tris, cone_weight 0.5), and packed into flat global buffers
// the GPU pulls by device address. Content-hash disk cached (the shader-cache pattern).
//
// Global-index scheme: meshlet_vertices store GLOBAL vertex indices into the shared vertex heap, so
// the existing HeapSuballocator/streaming stays intact (no per-draw rebasing). meshlet_triangles are
// packed 8-bit local indices, 3 per triangle, u32-packed (one uint per triangle: bytes [i0,i1,i2,0]).
struct MeshletModel
{
    std::vector<GpuMeshlet> meshlets;         // global meshlet buffer (all draws, all LODs)
    std::vector<uint32_t> meshlet_vertices;   // global vertex-index remap heap
    std::vector<uint32_t> meshlet_triangles;  // packed local triangle indices (1 uint / triangle)
    std::vector<GpuDrawInfo> draws;           // per-draw table (transform-less; caller fills model + material)

    // Per-draw meshlet range in the global buffer (LOD0 offset + total across LODs), so the streamer
    // can gate + the caller can compute the global meshlet-id base for the HiZ visibility bitfield.
    struct DrawMeshletRange { uint32_t first_meshlet; uint32_t total_meshlets; };
    std::vector<DrawMeshletRange> draw_ranges;
    uint32_t total_meshlets = 0;

    // Build timing (reported by the caller).
    double build_ms = 0.0;
    bool from_cache = false;
};

// Meshletization happens at COOK time (string-asset-tools, bake.cpp — bake_scene), not at runtime.
// This header defines only MeshletModel: the in-memory form the scene loader fills and GeometryPass
// uploads.

}  // namespace string::render
