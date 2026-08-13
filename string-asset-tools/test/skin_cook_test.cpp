// Brief 23 M1 — the skin cook. Two halves:
//   - bake_scene's SkinSource handling (lockstep repack, animated-bound substitution, v4
//     sections round-tripping, determinism) over synthetic geometry, and
//   - the glTF front end (cook_skins + bake_gltf) over a real in-memory skinned .glb: the
//     skinned-node identity-transform rule, the joint remap, the `.anim` pack, and the
//     cook-twice byte-determinism of BOTH output files.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <string/anim/anim_pack.hpp>
#include <string/asset/cooked_format.hpp>
#include <string/asset/cooked_scene.hpp>
#include <string/asset/manifest.hpp>
#include <string/asset/tools/bake.hpp>
#include <string/asset/tools/gltf_skin.hpp>

using namespace string::asset;
using namespace string::asset::tools;

namespace
{

// --- Synthetic geometry (the bake_scene half) ----------------------------------------------

// A grid mesh whose vertex index is recoverable from BOTH streams: pos.x encodes the index in
// the vertex, joints encode it in the skin vertex. After any repack, streams that moved in
// lockstep still agree; a skipped branch or an off-by-one disagrees immediately.
struct SynthSkinned
{
    std::vector<string::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<GltfDraw> draws;
    SkinSource skin;
};

SkinVertex tagged_skin(uint32_t index)
{
    SkinVertex sv{};
    sv.joints[0] = static_cast<uint8_t>(index & 0xFF);
    sv.joints[1] = static_cast<uint8_t>((index >> 8) & 0xFF);
    sv.weights[0] = 255;
    return sv;
}

SynthSkinned make_synth(int n)
{
    SynthSkinned s;
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x)
        {
            const uint32_t index = static_cast<uint32_t>(s.vertices.size());
            string::Vertex v{};
            v.pos = glm::vec3(float(index), float((x * 7 + y * 13) % 5) * 0.1f, float(y));
            v.color = glm::vec3(1.0f);
            v.texCoord = glm::vec2(float(x) / n, float(y) / n);
            v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            v.tangent = 0x40100401u;
            s.vertices.push_back(v);
            s.skin.vertices.push_back(tagged_skin(index));
        }
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
        {
            const uint32_t a = uint32_t(y * (n + 1) + x);
            const uint32_t b = a + 1;
            const uint32_t c = a + uint32_t(n + 1);
            const uint32_t d = c + 1;
            s.indices.insert(s.indices.end(), { a, c, b, b, c, d });
        }
    GltfDraw draw{};
    draw.index_count = static_cast<uint32_t>(s.indices.size());
    draw.aabb_min = glm::vec3(0.0f);
    draw.aabb_max = glm::vec3(float(n * n), 1.0f, float(n));
    draw.skin = 0;
    s.draws.push_back(draw);

    CookedSkin skin{};
    skin.skeleton_hash = 0x1234;
    skin.joint_count = 2;
    s.skin.skins.push_back(skin);
    s.skin.inverse_bind = { glm::mat4(1.0f), glm::mat4(1.0f) };
    s.skin.joint_remap = { 0, 1 };
    s.skin.skin_anim_min.push_back(glm::vec3(-100.0f));
    s.skin.skin_anim_max.push_back(glm::vec3(100.0f));
    return s;
}

uint32_t vertex_tag(const string::Vertex& v) { return static_cast<uint32_t>(v.pos.x + 0.5f); }

void expect_lockstep(const CookedScene& scene)
{
    ASSERT_EQ(scene.skin_vertices.size(), scene.vertices.size());
    for (std::size_t i = 0; i < scene.vertices.size(); ++i)
    {
        const SkinVertex expect = tagged_skin(vertex_tag(scene.vertices[i]));
        EXPECT_EQ(0, std::memcmp(&scene.skin_vertices[i], &expect, sizeof(SkinVertex)))
            << "skin stream diverged from vertex stream at packed index " << i;
    }
}

// --- A minimal skinned .glb (the front-end half) --------------------------------------------
//
// Two joints (under an "Armature" carrying its own translation), one skinned triangle whose
// node has a DELIBERATE translation that must be ignored, one unskinned copy of the triangle
// whose translation must be kept, and one LINEAR translation clip on the second joint.

void put_u32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(uint8_t(v & 0xFF));
    out.push_back(uint8_t((v >> 8) & 0xFF));
    out.push_back(uint8_t((v >> 16) & 0xFF));
    out.push_back(uint8_t((v >> 24) & 0xFF));
}

void put_floats(std::vector<uint8_t>& out, std::initializer_list<float> values)
{
    for (const float f : values)
    {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        put_u32(out, bits);
    }
}

std::vector<uint8_t> build_skinned_glb()
{
    // BIN chunk, 4-aligned regions in order; offsets recorded as we go.
    std::vector<uint8_t> bin;
    const uint32_t pos_off = 0;   // 3 x vec3
    put_floats(bin, { 0, 0, 0,  1, 0, 0,  0, 1, 0 });
    const uint32_t nrm_off = static_cast<uint32_t>(bin.size());   // 3 x vec3
    put_floats(bin, { 0, 0, 1,  0, 0, 1,  0, 0, 1 });
    const uint32_t uv_off = static_cast<uint32_t>(bin.size());    // 3 x vec2
    put_floats(bin, { 0, 0,  1, 0,  0, 1 });
    const uint32_t joints_off = static_cast<uint32_t>(bin.size());  // 3 x u8vec4
    for (const uint8_t b : { 0, 0, 0, 0,   0, 1, 0, 0,   1, 0, 0, 0 }) bin.push_back(b);
    const uint32_t weights_off = static_cast<uint32_t>(bin.size()); // 3 x vec4
    put_floats(bin, { 1, 0, 0, 0,  0.5f, 0.5f, 0, 0,  1, 0, 0, 0 });
    const uint32_t idx_off = static_cast<uint32_t>(bin.size());     // 3 x u16 (+pad)
    for (const uint8_t b : { 0, 0, 1, 0, 2, 0, 0, 0 }) bin.push_back(b);
    const uint32_t ibm_off = static_cast<uint32_t>(bin.size());     // 2 x identity mat4
    for (int m = 0; m < 2; ++m)
        put_floats(bin, { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 });
    const uint32_t times_off = static_cast<uint32_t>(bin.size());   // 2 x float
    put_floats(bin, { 0.0f, 1.0f });
    const uint32_t trans_off = static_cast<uint32_t>(bin.size());   // 2 x vec3
    put_floats(bin, { 0, 1, 0,  0, 2, 0 });

    const auto view = [](uint32_t off, uint32_t len) {
        return "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) +
               ",\"byteLength\":" + std::to_string(len) + "}";
    };
    const std::string json = std::string("{\"asset\":{\"version\":\"2.0\"},") +
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]," +
        "\"bufferViews\":[" +
            view(pos_off, 36) + "," + view(nrm_off, 36) + "," + view(uv_off, 24) + "," +
            view(joints_off, 12) + "," + view(weights_off, 48) + "," + view(idx_off, 6) + "," +
            view(ibm_off, 128) + "," + view(times_off, 8) + "," + view(trans_off, 24) + "]," +
        "\"accessors\":["
            "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
              "\"min\":[0,0,0],\"max\":[1,1,0]},"
            "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
            "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
            "{\"bufferView\":3,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
            "{\"bufferView\":4,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
            "{\"bufferView\":5,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
            "{\"bufferView\":6,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"},"
            "{\"bufferView\":7,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\","
              "\"min\":[0],\"max\":[1]},"
            "{\"bufferView\":8,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}],"
        "\"meshes\":["
            "{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,"
              "\"JOINTS_0\":3,\"WEIGHTS_0\":4},\"indices\":5}]},"
            "{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
              "\"indices\":5}]}],"
        "\"skins\":[{\"inverseBindMatrices\":6,\"joints\":[1,2],\"name\":\"TestSkin\"}],"
        "\"animations\":[{\"name\":\"TestClip\","
            "\"samplers\":[{\"input\":7,\"interpolation\":\"LINEAR\",\"output\":8}],"
            "\"channels\":[{\"sampler\":0,\"target\":{\"node\":2,\"path\":\"translation\"}}]}],"
        "\"nodes\":["
            "{\"name\":\"Armature\",\"translation\":[5,0,0],\"children\":[1]},"
            "{\"name\":\"j0\",\"children\":[2]},"
            "{\"name\":\"j1\",\"translation\":[0,1,0]},"
            "{\"name\":\"SkinnedMesh\",\"mesh\":0,\"skin\":0,\"translation\":[9,9,9]},"
            "{\"name\":\"StaticMesh\",\"mesh\":1,\"translation\":[2,0,0]}],"
        "\"scenes\":[{\"nodes\":[0,3,4]}],\"scene\":0}";

    std::vector<uint8_t> json_bytes(json.begin(), json.end());
    while (json_bytes.size() % 4 != 0) json_bytes.push_back(' ');
    while (bin.size() % 4 != 0) bin.push_back(0);

    std::vector<uint8_t> glb;
    put_u32(glb, 0x46546C67);   // "glTF"
    put_u32(glb, 2);
    put_u32(glb, uint32_t(12 + 8 + json_bytes.size() + 8 + bin.size()));
    put_u32(glb, uint32_t(json_bytes.size()));
    put_u32(glb, 0x4E4F534A);   // "JSON"
    glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
    put_u32(glb, uint32_t(bin.size()));
    put_u32(glb, 0x004E4942);   // "BIN\0"
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

std::vector<uint8_t> read_file(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(in) << p;
    std::vector<uint8_t> bytes(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    return bytes;
}

// A scratch dir per test-suite run; the skinned .glb fixture is written here once.
std::filesystem::path fixture_glb()
{
    static const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "string_skin_cook_test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "skinned.glb";
    const std::vector<uint8_t> glb = build_skinned_glb();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(glb.data()), std::streamsize(glb.size()));
    return path;
}

}  // namespace

// --- Quantization -----------------------------------------------------------------------------

TEST(SkinQuantize, WeightsSumTo255)
{
    const glm::u16vec4 joints{ 3, 7, 11, 19 };
    for (const glm::vec4 w : { glm::vec4(0.4f, 0.3f, 0.2f, 0.1f),
                               glm::vec4(0.33f, 0.33f, 0.33f, 0.01f),
                               glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),      // unnormalized input
                               glm::vec4(0.7f, 0.1f, 0.1f, 0.1f) })
    {
        const SkinVertex sv = quantize_skin_vertex(joints, w);
        EXPECT_EQ(255, int(sv.weights[0]) + int(sv.weights[1]) + int(sv.weights[2]) + int(sv.weights[3]));
    }
}

TEST(SkinQuantize, SortsByDescendingWeight)
{
    const SkinVertex sv = quantize_skin_vertex({ 1, 2, 3, 4 }, { 0.1f, 0.2f, 0.4f, 0.3f });
    EXPECT_EQ(sv.joints[0], 3);
    EXPECT_EQ(sv.joints[1], 4);
    EXPECT_EQ(sv.joints[2], 2);
    EXPECT_EQ(sv.joints[3], 1);
    EXPECT_GE(sv.weights[0], sv.weights[1]);
    EXPECT_GE(sv.weights[1], sv.weights[2]);
    EXPECT_GE(sv.weights[2], sv.weights[3]);
}

TEST(SkinQuantize, DegenerateCanonicalizes)
{
    const SkinVertex sv = quantize_skin_vertex({ 9, 9, 9, 9 }, glm::vec4(0.0f));
    EXPECT_EQ(sv.joints[0], 0);
    EXPECT_EQ(sv.weights[0], 255);
    EXPECT_EQ(sv.joints[1], 0);
    EXPECT_EQ(sv.weights[1], 0);
}

TEST(SkinQuantize, ZeroWeightInfluenceWritesCanonicalPadding)
{
    const SkinVertex sv = quantize_skin_vertex({ 5, 6, 7, 8 }, { 1.0f, 0.0f, 0.0f, 0.0f });
    EXPECT_EQ(sv.joints[0], 5);
    EXPECT_EQ(sv.weights[0], 255);
    for (int i = 1; i < 4; ++i)
    {
        EXPECT_EQ(sv.joints[i], 0);
        EXPECT_EQ(sv.weights[i], 0);
    }
}

// --- bake_scene with a SkinSource ---------------------------------------------------------------

TEST(SkinBake, RoundTripSections)
{
    const SynthSkinned s = make_synth(8);
    const CookedScene baked = bake_scene(s.vertices, s.indices, s.draws, BakeParams{}, s.skin);
    const std::vector<uint8_t> blob = write_cooked(baked);
    CookedScene read;
    ASSERT_TRUE(read_cooked(blob, read));

    ASSERT_EQ(read.skin_vertices.size(), read.vertices.size());
    ASSERT_EQ(read.skins.size(), 1u);
    EXPECT_EQ(read.skins[0].skeleton_hash, 0x1234u);
    EXPECT_EQ(read.skins[0].joint_count, 2u);
    ASSERT_EQ(read.inverse_bind.size(), 2u);
    ASSERT_EQ(read.joint_remap.size(), 2u);
    ASSERT_EQ(read.draws.size(), 1u);
    EXPECT_EQ(read.draws[0].skin_plus_one, 1u);
    // The skinned draw's bounds are the ANIMATED bound, not the geometry AABB.
    EXPECT_EQ(read.draws[0].aabb_min, glm::vec3(-100.0f));
    EXPECT_EQ(read.draws[0].aabb_max, glm::vec3(100.0f));
    expect_lockstep(read);
}

TEST(SkinBake, LockstepRepackSurvivesChunkSplit)
{
    const SynthSkinned s = make_synth(24);   // enough triangles to force splitting
    const CookedScene baked =
        bake_scene(s.vertices, s.indices, s.draws, BakeParams{ .chunk_max_meshlets = 4 }, s.skin);
    ASSERT_GT(baked.draws.size(), 1u) << "budget did not split — the gathered branch is untested";
    expect_lockstep(baked);
    for (const CookedDraw& d : baked.draws) EXPECT_EQ(d.skin_plus_one, 1u);
}

TEST(SkinBake, DeterministicBytesWithSkin)
{
    const SynthSkinned s = make_synth(16);
    const BakeParams params{ .chunk_max_meshlets = 8 };
    const std::vector<uint8_t> a = write_cooked(bake_scene(s.vertices, s.indices, s.draws, params, s.skin));
    const std::vector<uint8_t> b = write_cooked(bake_scene(s.vertices, s.indices, s.draws, params, s.skin));
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size()));
}

TEST(SkinBake, StaticSceneHasEmptySkinSections)
{
    const SynthSkinned s = make_synth(4);
    const CookedScene baked = bake_scene(s.vertices, s.indices, s.draws, BakeParams{});   // no skin
    EXPECT_TRUE(baked.skin_vertices.empty());
    EXPECT_TRUE(baked.skins.empty());
    const std::vector<uint8_t> blob = write_cooked(baked);
    CookedScene read;
    ASSERT_TRUE(read_cooked(blob, read));
    EXPECT_TRUE(read.skin_vertices.empty());
    ASSERT_EQ(read.draws.size(), 1u);
    EXPECT_EQ(read.draws[0].skin_plus_one, 0u);
}

// --- The glTF front end over the skinned fixture ------------------------------------------------

TEST(SkinGltf, SkinnedNodeTransformIsIdentityStaticSiblingKeepsIts)
{
    const CookedScene scene = bake_gltf(fixture_glb(), BakeParams{});
    ASSERT_EQ(scene.draws.size(), 2u);
    // Draw order follows scene-node order: SkinnedMesh (node 3) then StaticMesh (node 4).
    const CookedDraw& skinned = scene.draws[0];
    const CookedDraw& fixed = scene.draws[1];
    EXPECT_EQ(skinned.skin_plus_one, 1u);
    EXPECT_EQ(skinned.transform, glm::mat4(1.0f)) << "skinned node transform must be IGNORED";
    EXPECT_EQ(fixed.skin_plus_one, 0u);
    EXPECT_EQ(fixed.transform[3], glm::vec4(2, 0, 0, 1)) << "static sibling keeps its transform";
}

TEST(SkinGltf, AnimPackWritesAndParses)
{
    const std::filesystem::path glb = fixture_glb();
    const CookedScene scene = bake_gltf(glb, BakeParams{});

    string::anim::AnimPack pack;
    ASSERT_TRUE(string::anim::read_anim_pack_file(anim_path_for(glb), pack));
    EXPECT_EQ(pack.header.clip_count, 1u);
    ASSERT_EQ(pack.clips().size(), 1u);
    EXPECT_STREQ(pack.clips()[0].name, "TestClip");
    EXPECT_FLOAT_EQ(pack.clips()[0].duration, 1.0f);
    // Skeleton = 2 skin joints + the Armature ancestor that carries a real transform.
    EXPECT_EQ(pack.header.joint_count, 3u);
    EXPECT_GT(pack.header.skeleton_bytes, 0u);

    // The cooked skin pairs with the pack, and its remap lands inside the skeleton.
    ASSERT_EQ(scene.skins.size(), 1u);
    EXPECT_EQ(scene.skins[0].skeleton_hash, pack.header.skeleton_hash);
    ASSERT_EQ(scene.joint_remap.size(), 2u);
    EXPECT_NE(scene.joint_remap[0], scene.joint_remap[1]);
    for (const uint32_t ozz_joint : scene.joint_remap)
        EXPECT_LT(ozz_joint, pack.header.joint_count);
    // The animated bound must be non-degenerate and enclose the clip's reach.
    ASSERT_EQ(scene.draws[0].skin_plus_one, 1u);
    EXPECT_LT(scene.draws[0].aabb_min.y, scene.draws[0].aabb_max.y);
}

TEST(SkinGltf, CookTwiceIsByteIdenticalIncludingAnimPack)
{
    const std::filesystem::path glb = fixture_glb();
    const std::vector<uint8_t> cooked_a = write_cooked(bake_gltf(glb, BakeParams{}));
    const std::vector<uint8_t> anim_a = read_file(anim_path_for(glb));
    const std::vector<uint8_t> cooked_b = write_cooked(bake_gltf(glb, BakeParams{}));
    const std::vector<uint8_t> anim_b = read_file(anim_path_for(glb));

    ASSERT_EQ(cooked_a.size(), cooked_b.size());
    EXPECT_EQ(0, std::memcmp(cooked_a.data(), cooked_b.data(), cooked_a.size()));
    ASSERT_FALSE(anim_a.empty());
    ASSERT_EQ(anim_a.size(), anim_b.size());
    EXPECT_EQ(0, std::memcmp(anim_a.data(), anim_b.data(), anim_a.size()));
}

TEST(SkinGltf, QuantizedStreamCoversEveryVertex)
{
    const CookedScene scene = bake_gltf(fixture_glb(), BakeParams{});
    ASSERT_EQ(scene.skin_vertices.size(), scene.vertices.size());
    for (const SkinVertex& sv : scene.skin_vertices)
        EXPECT_EQ(255, int(sv.weights[0]) + int(sv.weights[1]) + int(sv.weights[2]) + int(sv.weights[3]));
}

// The offline detector for the skin-index off-by-one that shreds a character at runtime: every
// meshlet-vertex entry of every skinned draw must land inside the skin stream (at file scope the
// rebase delta is zero — the stream is parallel to the heap) on a vertex whose weights sum to 255.
TEST(SkinGltf, MeshletVerticesLandInsideTheSkinStream)
{
    const CookedScene scene = bake_gltf(fixture_glb(), BakeParams{});
    for (const CookedDraw& d : scene.draws)
    {
        if (d.skin_plus_one == 0) continue;
        for (uint32_t l = 0; l < d.lod_count; ++l)
            for (uint32_t m = 0; m < d.lods[l].meshlet_count; ++m)
            {
                const GpuMeshlet& ml = scene.meshlets[d.lods[l].meshlet_offset + m];
                for (uint32_t v = 0; v < ml.vertex_count; ++v)
                {
                    const uint32_t gv = scene.meshlet_vertices[ml.vertex_offset + v];
                    ASSERT_LT(gv, scene.skin_vertices.size());
                    const SkinVertex& sv = scene.skin_vertices[gv];
                    ASSERT_EQ(255, int(sv.weights[0]) + int(sv.weights[1]) +
                                   int(sv.weights[2]) + int(sv.weights[3]));
                }
            }
    }
}
