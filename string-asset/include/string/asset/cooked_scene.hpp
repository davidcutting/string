#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <string/vulkan/render_data.hpp>

#include <string/asset/cooked_format.hpp>
#include <string/asset/geometry.hpp>

namespace string::asset
{

// ============================================================================
// The cooked scene, and the READ half of the pipeline
// ============================================================================
//
// string-asset is the half of the asset pipeline that SHIPS: it can load the engine's preferred
// offline format and nothing else. The importers (glTF/OBJ/...), the meshletizer, the texture cook,
// and the writer all live in string-asset-tools, which depends on this library — never the reverse.
// That arrow is what keeps a shipped client from linking fastgltf and meshoptimizer just to open a
// scene it already cooked.

// The in-memory cooked scene: the exact tables the writer serializes and the reader materializes.
// Draws carry LOD ranges + bounds + vertex windows, NOT material/transform-derived draw records —
// binding those is the renderer's job at load (see the note in geometry.hpp on where the line sits).
struct CookedScene
{
    std::vector<string::Vertex> vertices;        // shared vertex heap
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

// Parse a cooked blob back into a CookedScene. Returns false on a bad magic / version / stride
// mismatch / truncation (the caller then re-cooks). Does NOT throw on malformed input.
bool read_cooked(const std::vector<uint8_t>& blob, CookedScene& out);

// Load + parse a cooked file. Returns false if missing/unreadable/malformed.
bool read_cooked_file(const std::filesystem::path& path, CookedScene& out);

}  // namespace string::asset
