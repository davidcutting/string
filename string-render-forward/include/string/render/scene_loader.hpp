#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <string/vulkan/render_data.hpp>

#include <string/render/gltf_types.hpp>
#include <string/render/meshlet_data.hpp>
#include <string/asset/tools/bake.hpp>

namespace string::render
{

// This is RENDERER-side, not part of the asset libraries, and deliberately so: merging N cooked
// scenes into one set of GpuDrawInfo arrays is "load into MY structures" — the layout it produces
// belongs to this renderer, not to the format. Keeping it here is what lets string-asset stay
// renderer-neutral (nothing in the asset libraries includes a renderer header).
//
// It does pull in string-asset-tools, for the in-process cook fallback below. That's a DEMO/editor
// convenience — a shipped client would load cooked-only and link string-asset alone.
using ::string::asset::CookedDraw;
using ::string::asset::CookedScene;

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
    std::vector<string::Vertex> vertices;
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

struct TextureCookSummary
{
    uint32_t cooked = 0;
    uint32_t skipped = 0;   // already fresh
    uint32_t failed = 0;
    double   ms = 0.0;
};

// Cook the KTX2/BC7 siblings for every texture these assets reference (brief 18).
//
// EXPLICIT AND BLOCKING BY DESIGN. Geometry cooking is cheap enough to happen implicitly on load;
// BC7 encoding is minutes, so doing it on load would read as a hang — and inside load_scene() it
// would sit in the middle of a device-idle stall. This is the "import" step: the user asks, it
// takes as long as it takes, and every subsequent load is fast (the streamer uploads BC7 blocks
// with no transcode, instead of the stb_image decode path).
//
// Idempotent: a texture whose .ktx2 is newer than its source is skipped, so re-running is cheap.
TextureCookSummary cook_scene_textures_for(const std::filesystem::path& resources_path,
                                           const std::vector<std::filesystem::path>& model_paths,
                                           uint32_t chunk_budget);

}  // namespace string::render
