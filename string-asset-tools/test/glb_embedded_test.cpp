// A .glb carries its images INSIDE the container. The cooked format addresses textures by PATH, so
// those images have to be extracted to files at bake — they used to be dropped with a warning, which
// silently lost the artist's textures and then crashed the engine, because the runtime decoder was
// handed a source with neither a file nor bytes and threw out of the scene build.
//
// These tests build a real .glb (header + JSON chunk + BIN chunk, with a genuine PNG in the BIN) and
// bake it, which is the only way to cover bake_gltf() — every other bake test works on an in-memory
// model and never touches the glTF front end.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <string/asset/tools/bake.hpp>
#include <string/asset/tools/gltf_loader.hpp>
#include <string/asset/cooked_format.hpp>
#include <string/core/png_writer.hpp>

using namespace string::asset;
using namespace string::asset::tools;

namespace
{

void put_u32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(uint8_t(v & 0xFF));
    out.push_back(uint8_t((v >> 8) & 0xFF));
    out.push_back(uint8_t((v >> 16) & 0xFF));
    out.push_back(uint8_t((v >> 24) & 0xFF));
}

// A 2x2 RGB PNG, produced by the engine's own encoder so the bytes are a real decodable image.
std::vector<uint8_t> tiny_png()
{
    const uint8_t pixels[2 * 2 * 3] = {
        255, 0, 0,   0, 255, 0,
        0, 0, 255,   255, 255, 0,
    };
    return string::core::png::encode(pixels, 2, 2, 3);
}

std::string base64(const std::vector<uint8_t>& bytes)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3)
    {
        const uint32_t a = bytes[i];
        const uint32_t b = (i + 1 < bytes.size()) ? bytes[i + 1] : 0u;
        const uint32_t c = (i + 2 < bytes.size()) ? bytes[i + 2] : 0u;
        const uint32_t triple = (a << 16) | (b << 8) | c;
        out += kAlphabet[(triple >> 18) & 0x3F];
        out += kAlphabet[(triple >> 12) & 0x3F];
        out += (i + 1 < bytes.size()) ? kAlphabet[(triple >> 6) & 0x3F] : '=';
        out += (i + 2 < bytes.size()) ? kAlphabet[triple & 0x3F] : '=';
    }
    return out;
}

// Assemble a .glb whose single image lives in the BIN chunk.
std::vector<uint8_t> build_glb(const std::vector<uint8_t>& image, const std::string& mime)
{
    const std::string mime_field = mime.empty() ? "" : ",\"mimeType\":\"" + mime + "\"";
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":" + std::to_string(image.size()) + "}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" + std::to_string(image.size()) + "}],"
        "\"images\":[{\"bufferView\":0" + mime_field + "}],"
        "\"samplers\":[{}],"
        "\"textures\":[{\"sampler\":0,\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"scenes\":[{\"nodes\":[]}],\"scene\":0}";

    // Both chunks pad to a 4-byte boundary: JSON with spaces, BIN with zeroes (glTF 2.0 spec).
    std::vector<uint8_t> json_bytes(json.begin(), json.end());
    while (json_bytes.size() % 4 != 0) json_bytes.push_back(' ');
    std::vector<uint8_t> bin_bytes = image;
    while (bin_bytes.size() % 4 != 0) bin_bytes.push_back(0);

    std::vector<uint8_t> glb;
    const uint32_t total = 12 + 8 + uint32_t(json_bytes.size()) + 8 + uint32_t(bin_bytes.size());
    glb.push_back('g'); glb.push_back('l'); glb.push_back('T'); glb.push_back('F');
    put_u32(glb, 2);
    put_u32(glb, total);
    put_u32(glb, uint32_t(json_bytes.size()));
    put_u32(glb, 0x4E4F534A);   // 'JSON'
    glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
    put_u32(glb, uint32_t(bin_bytes.size()));
    put_u32(glb, 0x004E4942);   // 'BIN\0'
    glb.insert(glb.end(), bin_bytes.begin(), bin_bytes.end());
    return glb;
}

// A scratch directory unique to the test, removed on scope exit.
struct TempDir
{
    std::filesystem::path path;
    explicit TempDir(const std::string& name)
    {
        path = std::filesystem::temp_directory_path() / ("string_glb_test_" + name);
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::filesystem::path write_glb(const TempDir& dir, const std::string& stem,
                                const std::vector<uint8_t>& glb)
{
    const std::filesystem::path file = dir.path / (stem + ".glb");
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(glb.data()), std::streamsize(glb.size()));
    return file;
}

}  // namespace

// The bug, directly: the cooked texture must carry a usable path, and the bytes must be on disk.
TEST(GlbEmbedded, ImageIsExtractedToAFileAndReferencedByPath)
{
    TempDir dir("extract");
    const std::vector<uint8_t> png = tiny_png();
    const std::filesystem::path glb = write_glb(dir, "embedded", build_glb(png, "image/png"));

    const CookedScene scene = bake_gltf(glb, BakeParams{});

    ASSERT_EQ(scene.textures.size(), 1u);
    // Was empty before the fix — that empty path is what reached the decoder and threw.
    ASSERT_NE(scene.textures[0].path[0], '\0') << "embedded image was dropped at bake";

    const std::filesystem::path extracted = glb.parent_path() / scene.textures[0].path;
    EXPECT_TRUE(std::filesystem::exists(extracted)) << extracted.string();
    EXPECT_EQ(std::filesystem::file_size(extracted), png.size());

    // Byte-identical to what went in: extraction must not re-encode.
    std::ifstream in(extracted, std::ios::binary);
    const std::vector<uint8_t> round_trip((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    EXPECT_EQ(round_trip, png);
}

// A base-colour map is sRGB; the flag has to survive extraction or the texture cooks in linear.
TEST(GlbEmbedded, ExtractedBaseColorKeepsSrgbFlag)
{
    TempDir dir("srgb");
    const std::filesystem::path glb =
        write_glb(dir, "embedded", build_glb(tiny_png(), "image/png"));

    const CookedScene scene = bake_gltf(glb, BakeParams{});

    ASSERT_EQ(scene.textures.size(), 1u);
    EXPECT_EQ(scene.textures[0].srgb, 1u);
}

// glTF REQUIRES mimeType on a buffer-view image, so that case cannot reach us — fastgltf rejects the
// file outright. What does reach us is an embedded data URI declared as a generic octet-stream,
// which several exporters emit: the mime type is then useless and the container has to be recognised
// from the magic bytes, or the image is unnameable and gets dropped exactly as before.
TEST(GlbEmbedded, DataUriWithGenericMimeTypeIsSniffedFromMagicBytes)
{
    TempDir dir("sniff");
    const std::vector<uint8_t> png = tiny_png();
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"images\":[{\"uri\":\"data:application/octet-stream;base64," + base64(png) + "\"}],"
        "\"samplers\":[{}],"
        "\"textures\":[{\"sampler\":0,\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"scenes\":[{\"nodes\":[]}],\"scene\":0}";

    const std::filesystem::path file = dir.path / "embedded.gltf";
    std::ofstream out(file, std::ios::binary);
    out.write(json.data(), std::streamsize(json.size()));
    out.close();

    const CookedScene scene = bake_gltf(file, BakeParams{});

    ASSERT_EQ(scene.textures.size(), 1u);
    ASSERT_NE(scene.textures[0].path[0], '\0') << "image with a generic mimeType was dropped";
    EXPECT_EQ(std::filesystem::path(scene.textures[0].path).extension(), ".png");
}

// Extraction is per-source-asset, so two .glb files in one folder cannot overwrite each other's
// images — they would otherwise both write image0.png beside themselves.
TEST(GlbEmbedded, TwoAssetsInOneFolderDoNotCollide)
{
    TempDir dir("collide");
    const std::vector<uint8_t> png = tiny_png();
    const std::filesystem::path a = write_glb(dir, "alpha", build_glb(png, "image/png"));
    const std::filesystem::path b = write_glb(dir, "beta", build_glb(png, "image/png"));

    const CookedScene sa = bake_gltf(a, BakeParams{});
    const CookedScene sb = bake_gltf(b, BakeParams{});

    ASSERT_EQ(sa.textures.size(), 1u);
    ASSERT_EQ(sb.textures.size(), 1u);
    EXPECT_STRNE(sa.textures[0].path, sb.textures[0].path);
}

// Re-baking must be idempotent: the artist re-exports constantly, and a stale or duplicated
// extraction would drift from the .glb it came from.
TEST(GlbEmbedded, RebakeIsIdempotent)
{
    TempDir dir("rebake");
    const std::filesystem::path glb =
        write_glb(dir, "embedded", build_glb(tiny_png(), "image/png"));

    const CookedScene first = bake_gltf(glb, BakeParams{});
    const CookedScene second = bake_gltf(glb, BakeParams{});

    ASSERT_EQ(first.textures.size(), 1u);
    ASSERT_EQ(second.textures.size(), 1u);
    EXPECT_STREQ(first.textures[0].path, second.textures[0].path);

    const std::filesystem::path dir_path = glb.parent_path() / (glb.stem().string() + ".textures");
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir_path)) { (void)entry; ++count; }
    EXPECT_EQ(count, 1u) << "re-bake duplicated the extracted image";
}
