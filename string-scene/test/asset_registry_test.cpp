#include <string/scene/asset_registry.hpp>

#include <gtest/gtest.h>

namespace
{

using namespace string::assets;
using string::asset::CookedDraw;
using string::asset::CookedMaterial;
using string::asset::CookedScene;
using string::asset::CookedSkin;
using string::asset::CookedTexture;
using string::asset::GpuMeshlet;
using string::asset::SkinVertex;

// A tiny synthetic cooked scene: v vertices, one meshlet, one draw, one material referencing
// texture 0, optionally one skin. File-LOCAL indices throughout — the registry's insert is the one
// place they become global, which is exactly what these tests pin.
CookedScene tiny_scene(uint32_t vertex_count, bool skinned)
{
    CookedScene s;
    s.vertices.resize(vertex_count);
    s.meshlet_vertices = { 0, 1, 2 };
    s.meshlet_triangles = { 0x00020100 };
    GpuMeshlet m{};
    m.vertex_offset = 0;
    m.triangle_offset = 0;
    m.vertex_count = 3;
    m.triangle_count = 1;
    s.meshlets.push_back(m);
    s.total_meshlets = 1;

    CookedTexture t{};
    t.path[0] = 'a';
    t.path[1] = '\0';
    t.srgb = 1;
    s.textures.push_back(t);

    CookedMaterial mat{};
    mat.base_color_factor = glm::vec4(1.0f);
    mat.base_color_texture = 0;      // file-local
    mat.metallic_roughness_texture = -1;
    mat.normal_texture = -1;
    mat.occlusion_texture = -1;
    s.materials.push_back(mat);

    CookedDraw d{};
    d.material = 0;                  // file-local
    d.first_meshlet = 0;
    d.total_meshlets = 1;
    d.lod_count = 1;
    d.lods[0].meshlet_offset = 0;
    d.lods[0].meshlet_count = 1;
    d.vertex_offset = 0;
    d.vertex_count = vertex_count;
    d.aabb_min = { -1, -1, -1 };
    d.aabb_max = { 1, 1, 1 };
    d.skin_plus_one = skinned ? 1 : 0;
    s.draws.push_back(d);

    if (skinned)
    {
        s.skin_vertices.resize(vertex_count, SkinVertex{ { 0, 0, 0, 0 }, { 255, 0, 0, 0 } });
        CookedSkin sk{};
        sk.skeleton_hash = 0xABCD;
        sk.joint_count = 2;
        sk.ibm_offset = 0;
        sk.remap_offset = 0;
        s.skins.push_back(sk);
        s.inverse_bind.resize(2, glm::mat4(1.0f));
        s.joint_remap = { 0, 1 };
    }
    return s;
}

TEST(AssetRegistry, RebasesFileLocalIndicesAtInsert)
{
    registry reg(registry_config{});
    const asset_id a = reg.load_baked(tiny_scene(10, false), "a");
    const asset_id b = reg.load_baked(tiny_scene(20, true), "b");
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());

    // Global tables accumulated.
    EXPECT_EQ(reg.vertices().size(), 30u);
    EXPECT_EQ(reg.meshlets().size(), 2u);
    EXPECT_EQ(reg.mesh_parts().size(), 2u);
    EXPECT_EQ(reg.materials().size(), 2u);
    EXPECT_EQ(reg.textures().size(), 2u);

    // Second file's rows rebased by the first file's bases.
    const mesh_part& p1 = reg.mesh_parts()[1];
    EXPECT_EQ(p1.first_meshlet, 1u);
    EXPECT_EQ(p1.lods[0].meshlet_offset, 1u);
    EXPECT_EQ(p1.vertex_offset, 10u);
    EXPECT_EQ(p1.material.index, 1u);
    // Meshlet-vertex remap rebased by the vertex base; meshlet offsets by the heap bases.
    EXPECT_EQ(reg.meshlet_vertices()[3], 10u);
    EXPECT_EQ(reg.meshlets()[1].vertex_offset, 3u);
    EXPECT_EQ(reg.meshlets()[1].triangle_offset, 1u);
    // Material's texture handle rebased to the global table.
    EXPECT_EQ(reg.materials()[1].base_color.index, 1u);

    // Skin binding: windows global, and the part's skin delta maps global vertex -> skin stream.
    ASSERT_EQ(reg.skins().size(), 1u);
    EXPECT_EQ(p1.skin.index, 0u);
    EXPECT_EQ(p1.skin_delta, -10);   // skin_vert_base(0) - vertex_base(10)
    EXPECT_EQ(reg.skins()[0].joint_count, 2u);
}

TEST(AssetRegistry, InterningReturnsTheExistingAsset)
{
    registry reg(registry_config{});
    const asset_id a = reg.load_baked(tiny_scene(4, false), "same");
    const asset_id b = reg.load_baked(tiny_scene(4, false), "same");
    EXPECT_EQ(a, b);
    EXPECT_EQ(reg.mesh_parts().size(), 1u);
    EXPECT_EQ(reg.loaded().size(), 1u);
}

TEST(AssetRegistry, MissingSourceFailsCleanWithoutProvider)
{
    registry reg(registry_config{});
    load_error err = load_error::none;
    const asset_id id = reg.load("/nonexistent/path/model.gltf", &err);
    EXPECT_FALSE(id.valid());
    EXPECT_EQ(err, load_error::not_found);
    EXPECT_TRUE(reg.mesh_parts().empty());
}

TEST(AssetRegistry, AssetRangesAndBounds)
{
    registry reg(registry_config{});
    reg.load_baked(tiny_scene(4, false), "a");
    const asset_id b = reg.load_baked(tiny_scene(4, false), "b");
    const asset* rec = reg.get(b);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->name(), "b");
    EXPECT_EQ(rec->first_mesh().index, 1u);
    EXPECT_EQ(rec->mesh_count(), 1u);
    EXPECT_EQ(rec->first_material().index, 1u);
    EXPECT_EQ(rec->first_texture().index, 1u);
    EXPECT_EQ(rec->bounds_min(), glm::vec3(-1.0f));
    EXPECT_EQ(rec->bounds_max(), glm::vec3(1.0f));
}

// v5 names: the cooked name blob survives the merge — parts/materials/skins carry the authored
// name; offset 0 (or a scene with no blob at all) reads as unnamed, never as garbage.
TEST(AssetRegistry, V5NamesSurviveTheMerge)
{
    CookedScene s = tiny_scene(10, true);
    const auto intern = [&s](std::string_view n) {
        if (s.names.empty()) s.names.push_back('\0');
        const auto off = static_cast<uint32_t>(s.names.size());
        s.names.insert(s.names.end(), n.begin(), n.end());
        s.names.push_back('\0');
        return off;
    };
    s.draws[0].name_offset = intern("hero_body");
    s.materials[0].name_offset = intern("hero_skin_mat");
    s.skins[0].name_offset = intern("hero_rig");

    registry reg(registry_config{});
    // An UNNAMED scene first, so the named one's parts land rebased (names must not be
    // offset-coupled to table position).
    reg.load_baked(tiny_scene(4, false), "plain");
    const asset_id id = reg.load_baked(std::move(s), "named");
    ASSERT_TRUE(id.valid());

    const asset* rec = reg.get(id);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(reg.mesh_parts()[rec->first_mesh().index].name, "hero_body");
    EXPECT_EQ(reg.materials()[rec->first_material().index].name, "hero_skin_mat");
    EXPECT_EQ(reg.skins()[rec->first_skin().index].name, "hero_rig");
    // The unnamed scene's records stay empty, not aliased into someone else's blob.
    EXPECT_TRUE(reg.mesh_parts()[0].name.empty());
}

}  // namespace
