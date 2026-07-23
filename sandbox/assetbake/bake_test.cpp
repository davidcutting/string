#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "bake.hpp"
#include "cooked_format.hpp"

using namespace sandbox;
using namespace sandbox::assetbake;

namespace
{

// Build a deterministic synthetic scene: two grid meshes (each a subdivided quad) as two draws over
// one shared vertex/index buffer, large enough to produce many meshlets + a multi-level LOD chain.
// No glTF / textures — bake_scene is the geometry-only core (the procgen-shaped entry point).
struct SynthScene
{
    std::vector<String::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<GltfDraw> draws;
};

void add_grid(SynthScene& s, int n, glm::vec3 origin, int material)
{
    const uint32_t base_vertex = static_cast<uint32_t>(s.vertices.size());
    const uint32_t index_offset = static_cast<uint32_t>(s.indices.size());
    glm::vec3 mn(1e30f), mx(-1e30f);
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x)
        {
            String::Vertex v{};
            v.pos = origin + glm::vec3(float(x), float((x * 7 + y * 13) % 5) * 0.1f, float(y));
            v.color = glm::vec3(1.0f);
            v.texCoord = glm::vec2(float(x) / n, float(y) / n);
            v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            v.tangent = 0x40100401u;   // arbitrary but fixed packed tangent
            s.vertices.push_back(v);
            mn = glm::min(mn, v.pos);
            mx = glm::max(mx, v.pos);
        }
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
        {
            const uint32_t a = base_vertex + uint32_t(y * (n + 1) + x);
            const uint32_t b = a + 1;
            const uint32_t c = a + uint32_t(n + 1);
            const uint32_t d = c + 1;
            s.indices.insert(s.indices.end(), { a, c, b, b, c, d });
        }
    GltfDraw draw{};
    draw.index_offset = index_offset;
    draw.index_count = static_cast<uint32_t>(s.indices.size()) - index_offset;
    draw.material = material;
    draw.transform = glm::mat4(1.0f);
    draw.aabb_min = mn;
    draw.aabb_max = mx;
    s.draws.push_back(draw);
}

SynthScene make_scene()
{
    SynthScene s;
    add_grid(s, 80, glm::vec3(0.0f), 0);
    add_grid(s, 60, glm::vec3(200.0f, 0.0f, 0.0f), 1);
    // An empty draw exercises the zero-triangle path.
    GltfDraw empty{};
    empty.material = -1;
    empty.transform = glm::mat4(1.0f);
    s.draws.push_back(empty);
    return s;
}

}  // namespace

// The bake produces a non-trivial scene (meshlets, a LOD chain, per-draw ranges).
TEST(BakeScene, ProducesMeshletsAndLods)
{
    const SynthScene s = make_scene();
    const CookedScene scene = bake_scene(s.vertices, s.indices, s.draws, BakeParams{});

    EXPECT_EQ(scene.vertices.size(), s.vertices.size());
    EXPECT_EQ(scene.draws.size(), s.draws.size());
    EXPECT_GT(scene.meshlets.size(), 0u);
    EXPECT_EQ(scene.total_meshlets, scene.meshlets.size());

    // The first (largest) draw should meshletize + build multiple LODs.
    const CookedDraw& d0 = scene.draws[0];
    EXPECT_GT(d0.total_meshlets, 0u);
    EXPECT_GT(d0.lod_count, 1u);
    EXPECT_EQ(d0.lods[0].error, 0.0f);           // LOD0 is the full mesh
    EXPECT_EQ(d0.first_meshlet, 0u);
    EXPECT_GT(d0.vertex_count, 0u);

    // The empty draw carries no meshlets.
    const CookedDraw& de = scene.draws.back();
    EXPECT_EQ(de.total_meshlets, 0u);
    EXPECT_EQ(de.lod_count, 0u);
}

// write -> read reproduces every table exactly (byte-for-byte on the POD sections).
TEST(BakeScene, RoundTrip)
{
    const SynthScene s = make_scene();
    CookedScene scene = bake_scene(s.vertices, s.indices, s.draws, BakeParams{});
    // Add materials + textures so those sections round-trip too.
    CookedMaterial m{};
    m.base_color_factor = glm::vec4(0.5f, 0.6f, 0.7f, 1.0f);
    m.base_color_texture = 3;
    m.alpha_mode = static_cast<uint8_t>(CookedAlphaMode::Mask);
    m.alpha_cutoff = 0.42f;
    m.double_sided = 1;
    scene.materials.push_back(m);
    CookedTexture t{};
    t.srgb = 1;
    const char* p = "textures/foo_BaseColor.png";
    std::memcpy(t.path, p, std::strlen(p));
    scene.textures.push_back(t);

    const std::vector<uint8_t> blob = write_cooked(scene);
    CookedScene back;
    ASSERT_TRUE(read_cooked(blob, back));

    ASSERT_EQ(back.vertices.size(), scene.vertices.size());
    EXPECT_EQ(0, std::memcmp(back.vertices.data(), scene.vertices.data(),
                             scene.vertices.size() * sizeof(String::Vertex)));
    ASSERT_EQ(back.meshlets.size(), scene.meshlets.size());
    EXPECT_EQ(0, std::memcmp(back.meshlets.data(), scene.meshlets.data(),
                             scene.meshlets.size() * sizeof(GpuMeshlet)));
    ASSERT_EQ(back.meshlet_vertices.size(), scene.meshlet_vertices.size());
    EXPECT_EQ(back.meshlet_vertices, scene.meshlet_vertices);
    ASSERT_EQ(back.meshlet_triangles.size(), scene.meshlet_triangles.size());
    EXPECT_EQ(back.meshlet_triangles, scene.meshlet_triangles);
    ASSERT_EQ(back.draws.size(), scene.draws.size());
    EXPECT_EQ(0, std::memcmp(back.draws.data(), scene.draws.data(),
                             scene.draws.size() * sizeof(CookedDraw)));
    ASSERT_EQ(back.materials.size(), scene.materials.size());
    EXPECT_EQ(0, std::memcmp(back.materials.data(), scene.materials.data(),
                             scene.materials.size() * sizeof(CookedMaterial)));
    ASSERT_EQ(back.textures.size(), scene.textures.size());
    EXPECT_EQ(0, std::memcmp(back.textures.data(), scene.textures.data(),
                             scene.textures.size() * sizeof(CookedTexture)));
    EXPECT_EQ(back.source_content_hash, scene.source_content_hash);
    EXPECT_EQ(back.total_meshlets, scene.total_meshlets);
}

// Cooking twice from identical input yields byte-identical blobs (CI + client/server contract).
TEST(BakeScene, DeterministicBytes)
{
    const SynthScene s = make_scene();
    const CookedScene a = bake_scene(s.vertices, s.indices, s.draws, BakeParams{});
    const CookedScene b = bake_scene(s.vertices, s.indices, s.draws, BakeParams{});
    const std::vector<uint8_t> ba = write_cooked(a);
    const std::vector<uint8_t> bb = write_cooked(b);
    ASSERT_EQ(ba.size(), bb.size());
    EXPECT_EQ(ba, bb);
    EXPECT_EQ(a.source_content_hash, b.source_content_hash);
}

// Chunking splits an oversized draw into more, smaller draws with tighter bounds, still
// deterministic byte-for-byte, and covering the same vertices.
TEST(BakeScene, ChunkingSplitsDeterministically)
{
    const SynthScene s = make_scene();
    BakeParams params;
    params.chunk_max_meshlets = 8;   // tiny budget -> the grids split into several chunks each

    const CookedScene chunked = bake_scene(s.vertices, s.indices, s.draws, params);
    const CookedScene unchunked = bake_scene(s.vertices, s.indices, s.draws, BakeParams{});

    EXPECT_GT(chunked.draws.size(), unchunked.draws.size());   // more, smaller draws
    EXPECT_EQ(chunked.chunk_max_meshlets, 8u);

    // Deterministic: same params -> byte-identical.
    const CookedScene chunked2 = bake_scene(s.vertices, s.indices, s.draws, params);
    EXPECT_EQ(write_cooked(chunked), write_cooked(chunked2));

    // Every chunk stays under (roughly) budget at LOD0 (allow the estimate's slack).
    for (const CookedDraw& d : chunked.draws)
    {
        if (d.total_meshlets == 0) continue;
        EXPECT_LE(d.lods[0].meshlet_count, 64u) << "a chunk's LOD0 meshlet count blew past budget";
    }

    // The vertex stream is grouped per draw: windows are contiguous, non-overlapping, and their sum
    // IS the stream (the streamer heap is sized to the window sum — a chunk that windowed its
    // parent's whole span would explode it). Meshlet-vertex remap entries stay inside their window.
    for (const CookedScene* sc : { &chunked, &unchunked })
    {
        uint64_t window_sum = 0;
        uint32_t expect_offset = 0;
        for (const CookedDraw& d : sc->draws)
        {
            if (d.vertex_count == 0) continue;
            EXPECT_EQ(d.vertex_offset, expect_offset) << "windows must tile the stream in draw order";
            expect_offset += d.vertex_count;
            window_sum += d.vertex_count;
            for (uint32_t l = 0; l < d.lod_count; ++l)
                for (uint32_t m = 0; m < d.lods[l].meshlet_count; ++m)
                {
                    const GpuMeshlet& ml = sc->meshlets[d.lods[l].meshlet_offset + m];
                    for (uint32_t v = 0; v < ml.vertex_count; ++v)
                    {
                        const uint32_t g = sc->meshlet_vertices[ml.vertex_offset + v];
                        EXPECT_GE(g, d.vertex_offset);
                        EXPECT_LT(g, d.vertex_offset + d.vertex_count);
                    }
                }
        }
        EXPECT_EQ(window_sum, sc->vertices.size());
    }
}

// Reader rejects a wrong-magic / truncated blob instead of throwing (caller then re-cooks).
TEST(BakeScene, ReaderRejectsGarbage)
{
    CookedScene out;
    std::vector<uint8_t> tiny(8, 0);
    EXPECT_FALSE(read_cooked(tiny, out));

    const SynthScene s = make_scene();
    std::vector<uint8_t> blob = write_cooked(bake_scene(s.vertices, s.indices, s.draws, BakeParams{}));
    blob[0] ^= 0xFF;   // corrupt the magic
    EXPECT_FALSE(read_cooked(blob, out));
}
