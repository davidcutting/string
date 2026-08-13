// Brief 23 M2 — string::anim: pack loading, sampling, blending, cross-fade, palette math.
// The committed fixture (data/two_joint.anim) is the pack the skin-cook test's in-memory
// skinned .glb produces: skeleton Armature(x+5) -> j0 -> j1(y+1), one clip "TestClip"
// (duration 1s, LINEAR translation on j1: (0,1,0) -> (0,2,0)). Regenerate by running
// skin_cook_tests and copying /tmp/string_skin_cook_test/skinned.anim.

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <string/anim/anim.hpp>

using namespace string::anim;

namespace
{

std::filesystem::path fixture_path()
{
    return std::filesystem::path(STRING_TEST_DATA_DIR) / "two_joint.anim";
}

std::shared_ptr<AnimSet> fixture_set()
{
    auto set = AnimSet::load(fixture_path());
    EXPECT_NE(set, nullptr) << "missing fixture " << fixture_path();
    return set;
}

// ozz's runtime Animation stores LOSSY-compressed keys, so sampled poses are only accurate
// to ~1e-3 of the value — a property to design around (palette precision), not a bug. Pure
// matrix math (build_palette, CrossFade weights) stays exact and uses tighter tolerances.
constexpr float kSampledPoseEps = 5e-3f;

void expect_near(const glm::vec3& a, const glm::vec3& b, float eps = kSampledPoseEps)
{
    EXPECT_NEAR(a.x, b.x, eps);
    EXPECT_NEAR(a.y, b.y, eps);
    EXPECT_NEAR(a.z, b.z, eps);
}

}  // namespace

TEST(AnimSet, LoadsFixturePack)
{
    const auto set = fixture_set();
    ASSERT_NE(set, nullptr);
    ASSERT_NE(set->skeleton(), nullptr);
    EXPECT_EQ(set->skeleton()->joint_count(), 3u);   // Armature + j0 + j1
    EXPECT_NE(set->skeleton_hash(), 0u);
    ASSERT_EQ(set->clips().size(), 1u);
    EXPECT_NE(set->find("TestClip"), nullptr);
    EXPECT_EQ(set->find("NoSuchClip"), nullptr);
    EXPECT_FLOAT_EQ(set->find("TestClip")->duration(), 1.0f);
    EXPECT_GE(set->skeleton()->find_joint("j1"), 0);
    EXPECT_EQ(set->skeleton()->find_joint("nope"), -1);
}

TEST(AnimSet, RejectsGarbage)
{
    EXPECT_EQ(AnimSet::load(fixture_path().parent_path() / "does_not_exist.anim"), nullptr);
    // A structurally valid pack whose blobs are not ozz archives must reject, not crash:
    // Skeleton::from_blob on arbitrary bytes.
    const uint8_t junk[64] = { 1, 2, 3 };
    EXPECT_EQ(Skeleton::from_blob({ junk, sizeof(junk) }), nullptr);
}

TEST(AnimPlayer, SamplesKnownPose)
{
    const auto set = fixture_set();
    const Clip* clip = set->find("TestClip");
    const int32_t j1 = set->skeleton()->find_joint("j1");
    ASSERT_GE(j1, 0);

    AnimPlayer player(set->skeleton());
    // t=0: j1 local translation (0,1,0); model space adds Armature's (5,0,0).
    player.set_layers(std::array{ AnimPlayer::Layer{ clip, 0.0f, 1.0f } });
    player.sample();
    ASSERT_EQ(player.model_space().size(), 3u);
    expect_near(glm::vec3(player.model_space()[j1][3]), { 5.0f, 1.0f, 0.0f });

    // t=duration: the channel's last key (0,2,0).
    player.set_layers(std::array{ AnimPlayer::Layer{ clip, 1.0f, 1.0f } });
    player.sample();
    expect_near(glm::vec3(player.model_space()[j1][3]), { 5.0f, 2.0f, 0.0f });
}

TEST(AnimPlayer, RestPoseWhenNoLayers)
{
    const auto set = fixture_set();
    AnimPlayer player(set->skeleton());
    player.set_layers({});
    player.sample();
    const int32_t j1 = set->skeleton()->find_joint("j1");
    ASSERT_GE(j1, 0);
    // Rest pose: Armature(5,0,0) * j0(identity) * j1(0,1,0).
    expect_near(glm::vec3(player.model_space()[j1][3]), { 5.0f, 1.0f, 0.0f });
}

TEST(AnimPlayer, BlendsHalfway)
{
    const auto set = fixture_set();
    const Clip* clip = set->find("TestClip");
    const int32_t j1 = set->skeleton()->find_joint("j1");

    AnimPlayer player(set->skeleton());
    // Same clip at its two ends, equal weights -> the midpoint translation y=1.5.
    player.set_layers(std::array{ AnimPlayer::Layer{ clip, 0.0f, 0.5f },
                                  AnimPlayer::Layer{ clip, 1.0f, 0.5f } });
    player.sample();
    expect_near(glm::vec3(player.model_space()[j1][3]), { 5.0f, 1.5f, 0.0f });

    // Unnormalized weights normalize (ozz divides by the accumulated weight)...
    player.set_layers(std::array{ AnimPlayer::Layer{ clip, 0.0f, 2.0f },
                                  AnimPlayer::Layer{ clip, 1.0f, 2.0f } });
    player.sample();
    expect_near(glm::vec3(player.model_space()[j1][3]), { 5.0f, 1.5f, 0.0f });

    // ...and a zero-weight layer is dropped entirely (single-layer fast path, exact pose).
    player.set_layers(std::array{ AnimPlayer::Layer{ clip, 0.0f, 1.0f },
                                  AnimPlayer::Layer{ clip, 1.0f, 0.0f } });
    player.sample();
    expect_near(glm::vec3(player.model_space()[j1][3]), { 5.0f, 1.0f, 0.0f });
}

TEST(CrossFade, TimingAndHandoff)
{
    const auto set = fixture_set();
    // Two DISTINCT Clip instances so play() sees a real transition (same pointer = no-op).
    const Clip* a = set->find("TestClip");
    const auto set2 = AnimSet::load(fixture_path());
    const Clip* b = set2->find("TestClip");
    ASSERT_NE(a, b);

    CrossFade fade;
    fade.play(a, 0.0f);
    ASSERT_EQ(fade.layers().size(), 1u);
    EXPECT_EQ(fade.layers()[0].clip, a);
    EXPECT_FLOAT_EQ(fade.layers()[0].weight, 1.0f);

    fade.advance(0.6f);
    EXPECT_FLOAT_EQ(fade.layers()[0].time, 0.6f);

    fade.play(b, 0.25f);
    ASSERT_EQ(fade.layers().size(), 2u);
    EXPECT_EQ(fade.layers()[0].clip, b);
    EXPECT_EQ(fade.layers()[1].clip, a);
    EXPECT_FLOAT_EQ(fade.layers()[1].time, 0.6f) << "outgoing layer must keep its pose time";

    fade.advance(0.125f);   // half the fade
    EXPECT_NEAR(fade.layers()[0].weight, 0.5f, 1e-4f);
    EXPECT_NEAR(fade.layers()[1].weight, 0.5f, 1e-4f);

    fade.advance(0.125f);   // fade complete -> outgoing dropped
    ASSERT_EQ(fade.layers().size(), 1u);
    EXPECT_EQ(fade.layers()[0].clip, b);
    EXPECT_FLOAT_EQ(fade.layers()[0].weight, 1.0f);

    // Looping wrap: 0.25 elapsed so far on b; +2.05 => 2.3 => wraps to 0.3 (duration 1s).
    fade.advance(2.05f);
    EXPECT_NEAR(fade.layers()[0].time, 0.3f, 1e-4f);

    fade.play(b, 0.25f);   // same clip -> no-op, no new layer
    EXPECT_EQ(fade.layers().size(), 1u);
}

TEST(Palette, RemapAndInverseBind)
{
    const glm::mat4 m0 = glm::translate(glm::mat4(1.0f), { 1.0f, 0.0f, 0.0f });
    const glm::mat4 m1 = glm::translate(glm::mat4(1.0f), { 0.0f, 2.0f, 0.0f });
    const std::array<glm::mat4, 2> model_space{ m0, m1 };
    const std::array<uint32_t, 2> remap{ 1, 0 };   // glTF joint 0 -> ozz joint 1, and vice versa
    // IBM chosen as the exact inverse of the remapped model transform -> identity palette.
    const std::array<glm::mat4, 2> ibm{ glm::inverse(m1), glm::inverse(m0) };
    std::array<glm::mat4, 2> palette{};
    build_palette(model_space, remap, ibm, palette);
    for (int i = 0; i < 2; ++i)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                EXPECT_NEAR(palette[i][c][r], glm::mat4(1.0f)[c][r], 1e-5f);

    // And the general product: identity IBMs give the remapped model matrices back.
    const std::array<glm::mat4, 2> id_ibm{ glm::mat4(1.0f), glm::mat4(1.0f) };
    build_palette(model_space, remap, id_ibm, palette);
    expect_near(glm::vec3(palette[0][3]), { 0.0f, 2.0f, 0.0f });
    expect_near(glm::vec3(palette[1][3]), { 1.0f, 0.0f, 0.0f });
}

// The real content pack, when present (gitignored asset — skips cleanly elsewhere).
TEST(AnimSet, UalPackIdentity)
{
    std::filesystem::path path =
        "../sandbox/assets/universal_animation_library/Unreal-Godot/UAL1_Standard.anim";
    if (const char* env = std::getenv("STRING_UAL1_ANIM")) path = env;
    if (!std::filesystem::exists(path)) GTEST_SKIP() << "UAL1 content not present";

    const auto set = AnimSet::load(path);
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->skeleton()->joint_count(), 66u);   // 65 skin joints + Armature ancestor
    EXPECT_EQ(set->clips().size(), 43u);
    EXPECT_NE(set->find("Idle_Loop"), nullptr);
    EXPECT_NE(set->find("Walk_Loop"), nullptr);
    EXPECT_GT(set->find("Walk_Loop")->duration(), 0.0f);
}
