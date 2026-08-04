// Texture-cook gates. Two properties matter and both are invisible at the call site:
//   1. The output is BC7 ALREADY — if it still needs transcoding, the runtime pays a per-load
//      transcode and the whole point of baking BC7 to disk is lost.
//   2. The transfer function follows the caller's srgb flag. This is the bug the old shell script
//      could not avoid (it guessed from the filename), so it is the thing most worth pinning.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>
#include <ktx.h>
#include <volk.h>

#include <string/asset/tools/texture_cook.hpp>
#include <string/core/png_writer.hpp>

using namespace string::asset::tools;

namespace
{

// A 64x64 RGBA gradient, written as a real PNG so the cook exercises its actual stb_image path.
std::filesystem::path write_test_png(const std::filesystem::path& dir, const char* name)
{
    constexpr uint32_t kDim = 64;
    std::vector<uint8_t> pixels(kDim * kDim * 4);
    for (uint32_t y = 0; y < kDim; ++y)
        for (uint32_t x = 0; x < kDim; ++x)
        {
            const size_t i = (static_cast<size_t>(y) * kDim + x) * 4;
            pixels[i + 0] = static_cast<uint8_t>(x * 4);
            pixels[i + 1] = static_cast<uint8_t>(y * 4);
            pixels[i + 2] = static_cast<uint8_t>((x ^ y) * 4);
            pixels[i + 3] = 255;
        }
    const std::vector<uint8_t> png = string::core::png::encode(pixels.data(), kDim, kDim, 4);
    const std::filesystem::path path = dir / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return path;
}

std::filesystem::path scratch_dir()
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "string_texture_cook_test";
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST(TextureCook, BakesBc7NeedingNoTranscode)
{
    const std::filesystem::path dir = scratch_dir();
    const std::filesystem::path src = write_test_png(dir, "color.png");
    std::filesystem::path out = src;
    out.replace_extension(".ktx2");
    std::filesystem::remove(out);

    TextureCookParams params;
    params.zstd_level = 1;   // the gate is about format, not compression ratio; keep it quick
    ASSERT_TRUE(cook_texture(src, out, /*srgb=*/true, params));
    ASSERT_TRUE(std::filesystem::exists(out));

    ktxTexture2* tex = nullptr;
    ASSERT_EQ(ktxTexture2_CreateFromNamedFile(out.string().c_str(), 0, &tex), KTX_SUCCESS);
    EXPECT_FALSE(ktxTexture2_NeedsTranscoding(tex));
    EXPECT_EQ(tex->vkFormat, static_cast<ktx_uint32_t>(VK_FORMAT_BC7_SRGB_BLOCK));
    // 64x64 => levels 64,32,16,8,4,2,1. A truncated chain would make the streamer's coarse-tail
    // LOD fall off a cliff, so pin the full chain.
    EXPECT_EQ(tex->numLevels, 7u);
    EXPECT_EQ(tex->baseWidth, 64u);
    EXPECT_EQ(tex->baseHeight, 64u);
    ktxTexture_Destroy(ktxTexture(tex));
}

TEST(TextureCook, LinearFlagPicksUnormNotSrgb)
{
    const std::filesystem::path dir = scratch_dir();
    const std::filesystem::path src = write_test_png(dir, "normal.png");
    std::filesystem::path out = src;
    out.replace_extension(".ktx2");
    std::filesystem::remove(out);

    TextureCookParams params;
    params.zstd_level = 1;
    ASSERT_TRUE(cook_texture(src, out, /*srgb=*/false, params));

    ktxTexture2* tex = nullptr;
    ASSERT_EQ(ktxTexture2_CreateFromNamedFile(out.string().c_str(), 0, &tex), KTX_SUCCESS);
    // The filename says "normal", but nothing in the cook reads the filename — only the flag does.
    EXPECT_EQ(tex->vkFormat, static_cast<ktx_uint32_t>(VK_FORMAT_BC7_UNORM_BLOCK));
    ktxTexture_Destroy(ktxTexture(tex));
}

TEST(TextureCook, SkipsWhenUpToDateAndReCooksOnForce)
{
    const std::filesystem::path dir = scratch_dir();
    const std::filesystem::path src = write_test_png(dir, "incremental.png");
    std::filesystem::path out = src;
    out.replace_extension(".ktx2");
    std::filesystem::remove(out);

    TextureCookParams params;
    params.zstd_level = 1;
    ASSERT_TRUE(cook_texture(src, out, true, params));
    const auto first = std::filesystem::last_write_time(out);

    // Second call with the output newer than the source must not touch the file.
    ASSERT_TRUE(cook_texture(src, out, true, params));
    EXPECT_EQ(std::filesystem::last_write_time(out), first);

    params.force = true;
    ASSERT_TRUE(cook_texture(src, out, true, params));
    EXPECT_NE(std::filesystem::last_write_time(out), first);
}

TEST(TextureCook, MissingSourceFailsRatherThanWritingGarbage)
{
    const std::filesystem::path dir = scratch_dir();
    const std::filesystem::path src = dir / "does_not_exist.png";
    const std::filesystem::path out = dir / "does_not_exist.ktx2";
    std::filesystem::remove(out);

    EXPECT_FALSE(cook_texture(src, out, true, TextureCookParams{}));
    EXPECT_FALSE(std::filesystem::exists(out));
}
