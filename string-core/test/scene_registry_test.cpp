#include <gtest/gtest.h>

#include <string/vulkan/scene_registry.hpp>

// Rescan is reachable only from a menu click, so these cover the parts a click cannot show:
// that a rescan drops exactly the discovered scenes, keeps the built-ins, and never drops the
// scene currently on screen (whose configure function a later reload still needs).

namespace
{

String::RenderPlan::ConfigureFn noop_configure()
{
    return [](String::engine_context&) { return String::RenderPlan::Setup{}; };
}

// The registry is a process singleton, so each test starts from a known state by re-adding what it
// needs and rescanning to a scanner that registers a controlled set.
void reset(String::SceneRegistry& r)
{
    r.set_rescan([] {});
    r.set_active("");
    r.rescan();   // drops any content scenes a previous test left behind
}

}  // namespace

TEST(SceneRegistry, RescanReplacesContentScenesButKeepsBuiltIns)
{
    String::SceneRegistry& r = String::SceneRegistry::instance();
    reset(r);

    r.add("ui", "built-in", noop_configure(), {}, /*from_content=*/false);
    r.add("old_asset", "content", noop_configure(), {}, /*from_content=*/true);
    ASSERT_NE(r.find("old_asset"), nullptr);

    r.set_rescan([] {
        String::SceneRegistry::instance().add("new_asset", "content",
                                              noop_configure(), {}, /*from_content=*/true);
    });
    r.rescan();

    EXPECT_NE(r.find("ui"), nullptr) << "a rescan must not drop built-in scenes";
    EXPECT_EQ(r.find("old_asset"), nullptr) << "a removed asset should leave the list";
    EXPECT_NE(r.find("new_asset"), nullptr) << "a newly dropped-in asset should appear";
}

TEST(SceneRegistry, RescanKeepsTheActiveSceneEvenIfItsAssetVanished)
{
    String::SceneRegistry& r = String::SceneRegistry::instance();
    reset(r);

    r.add("loaded_asset", "content", noop_configure(), {}, /*from_content=*/true);
    r.set_active("loaded_asset");

    // Scanner finds nothing — simulating the artist deleting or moving the file while it is loaded.
    r.set_rescan([] {});
    r.rescan();

    EXPECT_NE(r.find("loaded_asset"), nullptr)
        << "the scene on screen must survive a rescan; dropping its configure strands a reload";
}

TEST(SceneRegistry, RescanIsANoOpWithoutAScanner)
{
    String::SceneRegistry& r = String::SceneRegistry::instance();
    reset(r);
    r.add("content_scene", "content", noop_configure(), {}, /*from_content=*/true);

    r.set_rescan({});
    EXPECT_FALSE(r.can_rescan());
    r.rescan();

    EXPECT_NE(r.find("content_scene"), nullptr)
        << "with no scanner installed, rescan must not delete what is already registered";
}
