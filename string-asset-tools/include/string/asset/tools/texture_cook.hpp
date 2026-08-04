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
// This was `tools/cook_textures.sh` (shelling out to the `ktx` CLI). It is a library stage now for
// two reasons. The mechanical one: the geometry cook was C++ with a manifest and the texture cook
// was a shell script with none, so "cook this asset" had two entry points and two staleness stories.
// The substantive one is a correctness fix the script itself called out — it had to guess each
// image's transfer function from its FILENAME (`_normal`, `_orm`, ...) because a shell script can't
// read glTF. The cook can: `CookedTexture::srgb` is set from the image's actual material role
// (base-color is sRGB, every data map is linear). Guessing wrong means a normal map decoded through
// an sRGB curve, which is a subtle, permanent shading error.
//
// Output shape is unchanged, and deliberately so: BC7 baked straight to disk, zstd-supercompressed,
// so the runtime streamer uploads blocks with no transcode (see the renderer's texture_streamer —
// that is what removes the ~15s stb_image decode floor on Sponza). BC7 cannot be encoded directly,
// so this cooks in the same two stages the script did: encode UASTC, then transcode to BC7.

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
