#pragma once

#include <cstdint>
#include <filesystem>

#include <string/asset/cooked_scene.hpp>

namespace string::asset::tools
{

// ============================================================================
// Texture cook: source image -> KTX2/BC7 with a full mip chain
// ============================================================================
//
// A library stage, sharing the geometry cook's manifest and staleness story — one entry point for
// "cook this asset".
//
// TRANSFER FUNCTION COMES FROM THE MATERIAL ROLE, never from the filename: `CookedTexture::srgb` is
// set from how the glTF actually uses the image (base-color is sRGB, every data map is linear).
// Guessing from a `_normal`/`_orm` suffix decodes a normal map through an sRGB curve — a subtle,
// permanent shading error.
//
// BC7 is baked straight to disk, zstd-supercompressed, so the runtime streamer uploads blocks with
// no transcode (see texture_streamer — that is what removes the ~15s stb_image decode floor on
// Sponza). BC7 cannot be encoded directly, so this cooks in two stages: UASTC, then transcode.

struct TextureCookParams
{
    // zstd supercompression level applied per mip level. 18 matches the shell script; KTX2 deflates
    // each level independently, so the streamer inflates only the levels it actually reads.
    int zstd_level = 18;
    // Encoder threads. 0 => std::thread::hardware_concurrency().
    uint32_t threads = 0;
    // Re-cook even when the output is newer than the source.
    bool force = false;
};

// Cook one source image to `out` (KTX2/BC7, full mip chain). `srgb` is the image's transfer
// function, which the CALLER knows from the material role — this function never guesses.
// Returns false and logs on failure. Skips (returns true) when `out` is newer than `src` and
// `params.force` is unset.
bool cook_texture(const std::filesystem::path& src, const std::filesystem::path& out, bool srgb,
                  const TextureCookParams& params);

struct TextureCookStats
{
    uint32_t cooked = 0;
    uint32_t skipped = 0;
    uint32_t failed = 0;
};

// Cook every texture a CookedScene references, using the srgb flag baked from glTF. Paths in the
// scene are relative to the source asset's directory, so `base_dir` is that directory. Each source
// image gets a `.ktx2` sibling — the runtime prefers that sibling and falls back to stb_image, so
// cooking stays incremental and optional.
TextureCookStats cook_scene_textures(const CookedScene& scene,
                                     const std::filesystem::path& base_dir,
                                     const TextureCookParams& params);

}  // namespace string::asset::tools
