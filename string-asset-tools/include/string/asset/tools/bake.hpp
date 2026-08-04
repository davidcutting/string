#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <string/vulkan/render_data.hpp>

#include <string/asset/cooked_scene.hpp>
#include <string/asset/tools/gltf_loader.hpp>

namespace string::asset::tools
{

// ============================================================================
// string-asset-tools — the OFFLINE half of the asset pipeline
// ============================================================================
//
// Converts source assets into the engine's preferred offline format. This is a LIBRARY (procgen +
// the server share it), not a tool binary: its core entry is
//   bake_scene(vertices, indices, draws, params) -> CookedScene
// with the glTF importer (gltf_loader) as ONE front-end. Procgen calls bake_scene() with generated
// arrays; an OBJ/FBX front-end is another function that calls the same core. Deliberately NOT a
// plugin registry — bake_scene() is already the extension point, and a registry only earns its keep
// if importers ever need to be discovered at runtime.
//
// Everything here is tool-side. Nothing that ships in a client belongs in this library; the reader,
// the format, and the manifest live in string-asset, which this depends on.

// Bake parameters. Chunking splits oversized draws (the ivy problem) so draw-level frustum/shadow
// cull and per-draw LOD aren't defeated by draws whose bounds span the whole building.
struct BakeParams
{
    // Spatial chunking: draws whose LOD0 meshlet count exceeds this budget are recursively median-
    // split on triangle centroids (largest AABB axis) until each piece is under budget; each piece
    // becomes its own draw with tight bounds and its OWN LOD chain. 0 disables chunking.
    uint32_t chunk_max_meshlets = 0;
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

// --- Serialization (write half; the reader lives in string-asset) -----------

// Serialize a CookedScene into the little-endian, 16-aligned section blob (see cooked_format.hpp).
std::vector<uint8_t> write_cooked(const CookedScene& scene);

// Write the cooked blob to `out_path` atomically (temp-then-rename), creating parent dirs.
void write_cooked_file(const CookedScene& scene, const std::filesystem::path& out_path);

// FNV-1a content hash of the flatten output (vertices+indices+draws) — the source_content_hash the
// cook stamps and the loader compares for staleness. Exposed so the manifest can carry it.
uint64_t content_hash(const std::vector<String::Vertex>& vertices,
                      const std::vector<uint32_t>& indices,
                      const std::vector<GltfDraw>& draws);

}  // namespace string::asset::tools
