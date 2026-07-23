#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <string/vulkan/render_data.hpp>

#include "../gltf_loader.hpp"
#include "cooked_format.hpp"

namespace sandbox::assetbake
{

// ============================================================================
// Bake library (brief 04b)
// ============================================================================
//
// The cook logic is a LIBRARY (procgen + server share it), not a tool binary. Its core entry is
//   bake_scene(vertices, indices, draws, params) -> CookedScene
// with the glTF importer (gltf_loader) as ONE front-end: procgen will call bake_scene() with
// generated arrays. bake_scene absorbs what used to live in meshlet_builder (meshletize + LOD
// chain) plus the new spatial chunking; flatten/tangents stay in gltf_loader (the front-end).

// Bake parameters. Chunking splits oversized draws (the ivy problem) so draw-level frustum/shadow
// cull and per-draw LOD aren't defeated by draws whose bounds span the whole building.
struct BakeParams
{
    // Spatial chunking: draws whose LOD0 meshlet count exceeds this budget are recursively median-
    // split on triangle centroids (largest AABB axis) until each piece is under budget; each piece
    // becomes its own draw with tight bounds and its OWN LOD chain. 0 disables chunking.
    uint32_t chunk_max_meshlets = 0;
};

// The in-memory cooked scene: the exact tables the writer serializes and the reader materializes.
// bake_scene fills the geometry-only fields (draws carry LOD ranges + bounds + vertex windows, NOT
// material/transform-derived GpuDrawInfo — the engine fills those at load). The glTF front-end fills
// materials/textures (with per-file-rebased texture indices) before writing.
struct CookedScene
{
    std::vector<String::Vertex> vertices;        // shared vertex heap
    std::vector<GpuMeshlet> meshlets;            // global meshlet table
    std::vector<uint32_t> meshlet_vertices;      // global vertex-index remap
    std::vector<uint32_t> meshlet_triangles;     // packed local triangle words
    std::vector<CookedDraw> draws;               // per-draw table (bounds + LOD ranges + vertex window)
    std::vector<CookedMaterial> materials;
    std::vector<CookedTexture> textures;
    uint32_t total_meshlets = 0;
    uint64_t source_content_hash = 0;
    uint32_t chunk_max_meshlets = 0;             // the budget this scene was cooked with
};

// Core bake entry (procgen-shaped): meshletize + LOD-chain + optional spatial chunking of the
// flattened geometry. `vertices`/`indices`/`draws` are the flatten output (indices are transient —
// consumed here, never serialized). Returns a CookedScene with geometry sections filled and
// source_content_hash set; the CALLER fills materials/textures (front-end policy). Deterministic:
// identical inputs -> identical output arrays (padding zeroed, no iteration-order nondeterminism).
CookedScene bake_scene(const std::vector<String::Vertex>& vertices,
                       const std::vector<uint32_t>& indices,
                       const std::vector<GltfDraw>& draws,
                       const BakeParams& params);

// glTF front-end: parse + flatten + bake + fill materials/textures. Produces a complete CookedScene
// for one source glTF (textures reference paths relative to the glTF's directory). This is the
// importer both the CLI and the in-process fallback call.
CookedScene bake_gltf(const std::filesystem::path& gltf_path, const BakeParams& params);

// --- Serialization ---------------------------------------------------------

// Serialize a CookedScene into the little-endian, 16-aligned section blob (see cooked_format.hpp).
std::vector<uint8_t> write_cooked(const CookedScene& scene);

// Write the cooked blob to `out_path` atomically (temp-then-rename), creating parent dirs.
void write_cooked_file(const CookedScene& scene, const std::filesystem::path& out_path);

// Parse a cooked blob back into a CookedScene. Returns false on a bad magic / version / stride
// mismatch / truncation (the caller then re-cooks). Does NOT throw on malformed input.
bool read_cooked(const std::vector<uint8_t>& blob, CookedScene& out);

// Load + parse a cooked file. Returns false if missing/unreadable/malformed.
bool read_cooked_file(const std::filesystem::path& path, CookedScene& out);

// FNV-1a content hash of the flatten output (vertices+indices+draws) — the source_content_hash the
// cook stamps and the loader compares for staleness. Exposed so the manifest can carry it.
uint64_t content_hash(const std::vector<String::Vertex>& vertices,
                      const std::vector<uint32_t>& indices,
                      const std::vector<GltfDraw>& draws);

}  // namespace sandbox::assetbake
