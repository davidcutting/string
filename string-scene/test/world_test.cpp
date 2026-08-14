#include <string/scene/world.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

namespace
{

using namespace string::assets;
using namespace string::scene;

// A CPU-only registry with one tiny asset to spawn from.
struct world_fixture : ::testing::Test
{
    world_fixture() : reg(registry_config{}), w(reg)
    {
        CookedScene s;
        s.vertices.resize(3);
        string::asset::GpuMeshlet m{};
        m.vertex_count = 3;
        m.triangle_count = 1;
        s.meshlets.push_back(m);
        s.meshlet_vertices = { 0, 1, 2 };
        s.meshlet_triangles = { 0x00020100 };
        s.total_meshlets = 1;
        string::asset::CookedDraw d{};
        d.lod_count = 1;
        d.total_meshlets = 1;
        d.material = -1;
        d.aabb_min = { -1, -1, -1 };
        d.aabb_max = { 1, 1, 1 };
        s.draws.push_back(d);
        id = reg.load_baked(std::move(s), "fixture");
    }
    using CookedScene = string::asset::CookedScene;
    registry reg;
    world w;
    asset_id id;
};

TEST_F(world_fixture, SpawnPublishesRowsInSpawnOrder)
{
    const entity a = w.spawn({ .asset = id, .debug_name = "a" });
    const entity b = w.spawn({ .asset = id, .debug_name = "b", .server_id = 42 });
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());
    w.tick(0.0f);

    const frame_view fv = w.view();
    ASSERT_EQ(fv.instances.size(), 2u);
    EXPECT_EQ(fv.instances[0].entity_index, a.index);
    EXPECT_EQ(fv.instances[1].entity_index, b.index);
    EXPECT_EQ(w.name_of(b), "b");
    EXPECT_EQ(w.server_id_of(b), 42u);
}

TEST_F(world_fixture, DespawnInvalidatesTheHandleGenerationally)
{
    const entity a = w.spawn({ .asset = id });
    const uint64_t rev_before = w.view().revision;
    w.despawn(a);
    EXPECT_FALSE(w.valid(a));
    EXPECT_GT(w.view().revision, rev_before);

    // The slot is recycled with a NEW generation; the stale handle stays dead.
    const entity b = w.spawn({ .asset = id });
    EXPECT_EQ(b.index, a.index);
    EXPECT_NE(b.generation, a.generation);
    EXPECT_FALSE(w.valid(a));
    EXPECT_TRUE(w.valid(b));

    w.tick(0.0f);
    EXPECT_EQ(w.view().instances.size(), 1u);
}

TEST_F(world_fixture, ParentedTransformFlushesThroughTheHierarchy)
{
    const glm::mat4 root_t = glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0));
    const glm::mat4 child_t = glm::translate(glm::mat4(1.0f), glm::vec3(0, 5, 0));
    const entity root = w.spawn({ .asset = id, .transform = root_t });
    const entity child = w.spawn({ .asset = id, .transform = child_t, .parent = root });
    w.tick(0.0f);

    const glm::vec3 child_pos = glm::vec3(w.world_transform(child)[3]);
    EXPECT_FLOAT_EQ(child_pos.x, 10.0f);
    EXPECT_FLOAT_EQ(child_pos.y, 5.0f);

    // Moving the ROOT moves the child on the next flush.
    w.set_transform(root, glm::translate(glm::mat4(1.0f), glm::vec3(20, 0, 0)));
    w.tick(0.0f);
    EXPECT_FLOAT_EQ(glm::vec3(w.world_transform(child)[3]).x, 20.0f);
}

TEST_F(world_fixture, TransformOnlyUpdatesDoNotBumpTheRevision)
{
    const entity a = w.spawn({ .asset = id });
    w.tick(0.0f);
    const uint64_t rev = w.view().revision;
    w.set_transform(a, glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 3)));
    w.tick(0.0f);
    EXPECT_EQ(w.view().revision, rev);   // row SET unchanged — consumers skip their rebuild
    EXPECT_FLOAT_EQ(w.view().instances[0].transform[3].x, 1.0f);
}

TEST_F(world_fixture, LightsPublishGpuMirrors)
{
    w.add_light({ .type = string::LightType::POINT, .position = { 1, 2, 3 }, .range = 7.0f });
    w.tick(0.0f);
    ASSERT_EQ(w.view().lights.size(), 1u);
    EXPECT_FLOAT_EQ(w.view().lights[0].position_radius.w, 7.0f);
}

}  // namespace
