#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <string/vulkan/render_data.hpp>

#include "../gltf_loader.hpp"
#include "../passes/meshlet_data.hpp"
#include "bake.hpp"

namespace sandbox::assetbake
{

// The merged, engine-ready result of loading N cooked scenes (brief 04b). This is exactly the data
// the old GeometryPass constructor produced after parse+flatten+build_meshlets+merge, so the rest of
// the pass (build_meshlet_gpu, streamer, per-frame LOD/streaming) is unchanged:
//   - `vertices`         : merged shared vertex heap (streamer takes ownership)
//   - `meshlets/verts/tris`: merged global meshlet heaps (uploaded to GPU)
//   - `draws`            : geometry-only GpuDrawInfo records (LOD ranges + bounds; the pass fills
//                          material/transform), PLUS the per-draw vertex windows in `draw_windows`
//   - `materials`        : merged GltfMaterial table (texture indices rebased per file)
//   - `textures`         : merged GltfTexture list (path + srgb), for the pass's KTX/stb upload
//   - `draws_meta`       : merged GltfDraw records (transform, aabb, material, vertex/index window)
//                          — what the pass keeps in draws_ for per-frame culling + streamer set_draws
struct LoadedScene
{
    std::vector<String::Vertex> vertices;
    std::vector<GpuMeshlet> meshlets;
    std::vector<uint32_t> meshlet_vertices;
    std::vector<uint32_t> meshlet_triangles;
    std::vector<GpuDrawInfo> draws;          // geometry-only (lods/center/radius/first_meshlet/total)
    std::vector<GltfMaterial> materials;
    std::vector<GltfTexture> textures;
    std::vector<GltfDraw> draws_meta;        // transform/aabb/material + vertex+index window per draw
    uint32_t total_meshlets = 0;

    // Per-draw vertex window into `vertices` (min vertex + count), baked at cook (no runtime index
    // scan). The streamer suballocates + uploads exactly this range; parity with the old index-derived
    // [vmin, vmax+1].
    struct VertexWindow { uint32_t offset; uint32_t count; };
    std::vector<VertexWindow> draw_windows;

    // Load diagnostics.
    double load_ms = 0.0;
    uint32_t cooked_hits = 0;   // files served from a fresh cooked file
    uint32_t cooked_misses = 0; // files cooked in-process (missing/stale)
};

// Load + merge cooked scenes for `model_paths` (each relative to `resources_path`). For each file:
// resolve the cooked path (via the manifest / cooked_path_for); if the cooked file is missing or
// stale, cook IN-PROCESS via the bake library (WARN + CLI hint) and write it next to the source.
// Then merge all N cooked scenes into one LoadedScene with the same rebasing the old loader used
// (vertex/index/material/texture bases). `chunk_budget` selects the cooked variant (0 = chunking off).
LoadedScene load_cooked_scenes(const std::filesystem::path& resources_path,
                               const std::vector<std::filesystem::path>& model_paths,
                               uint32_t chunk_budget);

}  // namespace sandbox::assetbake
