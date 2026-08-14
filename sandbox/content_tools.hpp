#pragma once

#include <string/scene/asset_registry.hpp>

// Tool-backed content hooks for the demo app (dev/editor build). These are the pieces that DRIVE
// the importer/bake library, so they live app-side: the engine libraries never link
// string-asset-tools (a shipped client loads cooked-only).
namespace sandbox
{

// The registry's injected geometry cooker: bake in-process, best-effort write the cooked sidecar +
// manifest beside the source (a failed write is non-fatal — read-only content folders still load).
class tools_cook_provider final : public string::assets::cook_provider
{
public:
    bool cook_geometry(const std::filesystem::path& source, uint32_t chunk_budget,
                       string::asset::CookedScene& out) override;
};

// Brief 07: the standing material-probe (lookdev) scene — a roughness x metallic sphere grid, a
// white/mirror pair, and a neutral ground slab — generated in-process and baked through the same
// cook library as real content so the whole meshlet path is exercised.
string::assets::asset_id load_lookdev_asset(string::assets::registry& registry);

// Brief 04 (STRING_TRANSP_TEST=1): five alpha-blended quads at staggered depths + lateral offsets
// so the sorted transparency pass can be verified visually. Baked through the same library and
// loaded AFTER the scene's models so the merged table order matches the old in-pass injection.
string::assets::asset_id load_transp_test_asset(string::assets::registry& registry);

}  // namespace sandbox
