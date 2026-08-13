#include <gtest/gtest.h>

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>

// Brief 23 M0 link smoke for the offline half (ozz_animation_offline + its runtime deps):
// the builders live in ozz_animation_offline and produce runtime objects, so a minimal
// build-through proves all three archives resolve and link in dependency order. The cook's
// real skeleton/clip extraction (gltf_skin.cpp) is built on exactly this path.
TEST(OzzOffline, BuildsMinimalSkeletonAndClip)
{
    ozz::animation::offline::RawSkeleton raw_skeleton;
    raw_skeleton.roots.resize(1);
    raw_skeleton.roots[0].name = "root";
    raw_skeleton.roots[0].transform = ozz::math::Transform::identity();
    ASSERT_TRUE(raw_skeleton.Validate());

    ozz::animation::offline::SkeletonBuilder skeleton_builder;
    const ozz::unique_ptr<ozz::animation::Skeleton> skeleton = skeleton_builder(raw_skeleton);
    ASSERT_NE(skeleton, nullptr);
    EXPECT_EQ(skeleton->num_joints(), 1);

    ozz::animation::offline::RawAnimation raw_animation;
    raw_animation.duration = 1.0f;
    raw_animation.tracks.resize(1);
    raw_animation.tracks[0].translations.push_back(
        { 0.0f, ozz::math::Float3(0.0f, 1.0f, 0.0f) });
    ASSERT_TRUE(raw_animation.Validate());

    ozz::animation::offline::AnimationBuilder animation_builder;
    const ozz::unique_ptr<ozz::animation::Animation> animation = animation_builder(raw_animation);
    ASSERT_NE(animation, nullptr);
    EXPECT_FLOAT_EQ(animation->duration(), 1.0f);
    EXPECT_EQ(animation->num_tracks(), 1);
}
