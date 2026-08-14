#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace string::asset::tools
{

struct TextureCookSummary
{
    uint32_t cooked = 0;
    uint32_t skipped = 0;   // already fresh
    uint32_t failed = 0;
    double   ms = 0.0;
};

// Cook the KTX2/BC7 siblings for every texture these assets reference (brief 18). Moved out of the
// renderer (scene_loader) with the asset-layer split: this drives the importer + BC7 encoder, so it
// is tools-side by definition; the app's Scene-menu cook hook calls it.
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

}  // namespace string::asset::tools
