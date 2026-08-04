#include <string/asset/tools/texture_cook.hpp>

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

#include <ktx.h>
#include <volk.h>

#include <string/core/logger.hpp>

// Declarations-only elsewhere in the tree; this TU carries its own STATIC copy of the
// implementation so it cannot clash with the renderer's (geometry_pass.cpp also defines it, and
// both end up in the same link when the demo keeps the in-process cook fallback).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <string/core/stb_image.h>

namespace string::asset::tools
{
namespace
{

// sRGB <-> linear transfer, per the IEC 61966-2-1 piecewise curve. Mip reduction MUST happen in
// linear light for colour maps: averaging sRGB-encoded bytes directly darkens every mip, and the
// error compounds down the chain.
float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float c)
{
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// One 2x2 box reduction of an RGBA8 level. `srgb` decodes/encodes the three colour channels;
// alpha is always linear (it is coverage, never gamma-encoded). Odd dimensions clamp to the last
// row/column, so non-power-of-two sources reduce without reading out of bounds.
//
// Box is deliberate: it is the standard mip filter and is separable, cheap, and stable. Note this
// differs from `ktx create --generate-mipmap`, which defaults to a lanczos kernel — so a file
// re-cooked through this path will NOT be byte-identical to one cooked by the old shell script.
std::vector<uint8_t> reduce(const std::vector<uint8_t>& src, uint32_t sw, uint32_t sh, uint32_t dw,
                            uint32_t dh, bool srgb)
{
    std::vector<uint8_t> dst(static_cast<size_t>(dw) * dh * 4);
    for (uint32_t y = 0; y < dh; ++y)
    {
        const uint32_t y0 = std::min(y * 2, sh - 1);
        const uint32_t y1 = std::min(y * 2 + 1, sh - 1);
        for (uint32_t x = 0; x < dw; ++x)
        {
            const uint32_t x0 = std::min(x * 2, sw - 1);
            const uint32_t x1 = std::min(x * 2 + 1, sw - 1);
            const size_t s[4] = {
                (static_cast<size_t>(y0) * sw + x0) * 4, (static_cast<size_t>(y0) * sw + x1) * 4,
                (static_cast<size_t>(y1) * sw + x0) * 4, (static_cast<size_t>(y1) * sw + x1) * 4,
            };
            const size_t d = (static_cast<size_t>(y) * dw + x) * 4;
            for (uint32_t c = 0; c < 4; ++c)
            {
                const bool encoded = srgb && c < 3;
                float sum = 0.0f;
                for (size_t i = 0; i < 4; ++i)
                {
                    const float v = src[s[i] + c] / 255.0f;
                    sum += encoded ? srgb_to_linear(v) : v;
                }
                float avg = sum * 0.25f;
                if (encoded) avg = linear_to_srgb(avg);
                dst[d + c] = static_cast<uint8_t>(std::lround(std::clamp(avg, 0.0f, 1.0f) * 255.0f));
            }
        }
    }
    return dst;
}

// True when `out` exists and is at least as new as `src` (the incremental skip). Any I/O
// uncertainty answers "stale", so a doubtful file is re-cooked rather than silently kept.
bool up_to_date(const std::filesystem::path& src, const std::filesystem::path& out)
{
    std::error_code ec;
    if (!std::filesystem::exists(out, ec) || ec) return false;
    const auto out_time = std::filesystem::last_write_time(out, ec);
    if (ec) return false;
    const auto src_time = std::filesystem::last_write_time(src, ec);
    if (ec) return false;
    return out_time >= src_time;
}

}  // namespace

bool cook_texture(const std::filesystem::path& src, const std::filesystem::path& out, bool srgb,
                  const TextureCookParams& params)
{
    if (!params.force && up_to_date(src, out)) return true;

    int w = 0, h = 0, channels = 0;
    // Forced to 4 channels: BC7 encodes RGBA, and a uniform layout keeps the mip reducer simple.
    stbi_uc* pixels = stbi_load(src.string().c_str(), &w, &h, &channels, 4);
    if (!pixels || w <= 0 || h <= 0)
    {
        STRING_LOG_ERROR("[cook] texture: cannot read {}: {}", src.string(), stbi_failure_reason());
        if (pixels) stbi_image_free(pixels);
        return false;
    }

    std::vector<uint8_t> level(pixels, pixels + static_cast<size_t>(w) * h * 4);
    stbi_image_free(pixels);

    const auto base_w = static_cast<uint32_t>(w);
    const auto base_h = static_cast<uint32_t>(h);
    uint32_t levels = 1;
    for (uint32_t d = std::max(base_w, base_h); d > 1; d >>= 1) ++levels;

    ktxTextureCreateInfo info{};
    info.vkFormat = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    info.baseWidth = base_w;
    info.baseHeight = base_h;
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = levels;
    info.numLayers = 1;
    info.numFaces = 1;
    info.isArray = KTX_FALSE;
    info.generateMipmaps = KTX_FALSE;   // we supply every level ourselves, reduced in linear light

    ktxTexture2* tex = nullptr;
    KTX_error_code rc = ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex);
    if (rc != KTX_SUCCESS)
    {
        STRING_LOG_ERROR("[cook] texture: create failed for {}: {}", src.string(), ktxErrorString(rc));
        return false;
    }

    uint32_t lw = base_w, lh = base_h;
    for (uint32_t lvl = 0; lvl < levels; ++lvl)
    {
        if (lvl > 0)
        {
            const uint32_t nw = std::max(1u, lw / 2), nh = std::max(1u, lh / 2);
            level = reduce(level, lw, lh, nw, nh, srgb);
            lw = nw;
            lh = nh;
        }
        rc = ktxTexture_SetImageFromMemory(ktxTexture(tex), lvl, 0, 0, level.data(), level.size());
        if (rc != KTX_SUCCESS)
        {
            STRING_LOG_ERROR("[cook] texture: level {} failed for {}: {}", lvl, src.string(),
                             ktxErrorString(rc));
            ktxTexture_Destroy(ktxTexture(tex));
            return false;
        }
    }

    // Stage 1: UASTC. `ktx create --encode` can only produce Basis (UASTC/ETC1S), never raw BC7,
    // so UASTC is a throwaway intermediate here exactly as it was a temp file in the shell script.
    ktxBasisParams basis{};
    basis.structSize = sizeof(basis);
    basis.uastc = KTX_TRUE;
    basis.threadCount = params.threads ? params.threads
                                       : std::max(1u, std::thread::hardware_concurrency());
    rc = ktxTexture2_CompressBasisEx(tex, &basis);
    if (rc != KTX_SUCCESS)
    {
        STRING_LOG_ERROR("[cook] texture: UASTC encode failed for {}: {}", src.string(),
                         ktxErrorString(rc));
        ktxTexture_Destroy(ktxTexture(tex));
        return false;
    }

    // Stage 2: UASTC -> BC7. Baking BC7 to disk is what lets the runtime skip transcoding
    // entirely; String targets desktop Vulkan, so the universal intermediate buys nothing at load.
    rc = ktxTexture2_TranscodeBasis(tex, KTX_TTF_BC7_RGBA, 0);
    if (rc != KTX_SUCCESS)
    {
        STRING_LOG_ERROR("[cook] texture: BC7 transcode failed for {}: {}", src.string(),
                         ktxErrorString(rc));
        ktxTexture_Destroy(ktxTexture(tex));
        return false;
    }

    rc = ktxTexture2_DeflateZstd(tex, static_cast<ktx_uint32_t>(params.zstd_level));
    if (rc != KTX_SUCCESS)
    {
        STRING_LOG_ERROR("[cook] texture: zstd deflate failed for {}: {}", src.string(),
                         ktxErrorString(rc));
        ktxTexture_Destroy(ktxTexture(tex));
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    // Temp-then-rename, matching the cooked-scene writer: a crash mid-write must never leave a
    // truncated .ktx2 that the streamer would then happily try to upload.
    const std::filesystem::path tmp = out.string() + ".tmp";
    rc = ktxTexture_WriteToNamedFile(ktxTexture(tex), tmp.string().c_str());
    ktxTexture_Destroy(ktxTexture(tex));
    if (rc != KTX_SUCCESS)
    {
        STRING_LOG_ERROR("[cook] texture: write failed for {}: {}", out.string(), ktxErrorString(rc));
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, out, ec);
    if (ec)
    {
        STRING_LOG_ERROR("[cook] texture: rename failed for {}: {}", out.string(), ec.message());
        std::filesystem::remove(tmp, ec);
        return false;
    }

    STRING_LOG_INFO("[cook] texture ({}) {} -> {} ({}x{}, {} levels)", srgb ? "srgb" : "linear",
                    src.string(), out.string(), base_w, base_h, levels);
    return true;
}

TextureCookStats cook_scene_textures(const CookedScene& scene, const std::filesystem::path& base_dir,
                                     const TextureCookParams& params)
{
    TextureCookStats stats;
    for (const CookedTexture& t : scene.textures)
    {
        if (t.path[0] == '\0') continue;   // embedded image: no file to cook, runtime falls back

        const std::filesystem::path src = base_dir / std::filesystem::path(t.path);
        std::error_code ec;
        if (!std::filesystem::exists(src, ec) || ec)
        {
            STRING_LOG_WARN("[cook] texture: source missing, skipping: {}", src.string());
            ++stats.failed;
            continue;
        }
        // The runtime resolves a `.ktx2` SIBLING of the source path, so the output name is the
        // source with its extension replaced — not a separate output tree.
        std::filesystem::path out = src;
        out.replace_extension(".ktx2");

        if (!params.force && up_to_date(src, out))
        {
            ++stats.skipped;
            continue;
        }
        if (cook_texture(src, out, t.srgb != 0, params))
            ++stats.cooked;
        else
            ++stats.failed;
    }
    return stats;
}

}  // namespace string::asset::tools
