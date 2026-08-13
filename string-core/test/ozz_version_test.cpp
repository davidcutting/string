#include <gtest/gtest.h>

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>

// Brief 23: the cooked `.anim` packs carry opaque ozz::io archive blobs, so their on-disk
// compatibility is pinned by ozz's per-type archive versions, not by an ozz release number
// (ozz exposes no version macro). A bump here on an ozz upgrade means every pack re-cooks;
// this failing is the intended compile-time tripwire, mirroring bake.cpp's
// MESHOPTIMIZER_VERSION assert.
static_assert(ozz::io::internal::Version<const ozz::animation::Animation>::kValue == 7,
              "ozz Animation archive version changed: bump kAnimPackVersion and re-cook");
static_assert(ozz::io::internal::Version<const ozz::animation::Skeleton>::kValue == 2,
              "ozz Skeleton archive version changed: bump kAnimPackVersion and re-cook");
static_assert(ozz::animation::Skeleton::kMaxJoints == 1024);

// Link smoke for the runtime half (ozz_animation + ozz_base): Skeleton's ctor/dtor live in
// ozz_animation and its deallocation path in ozz_base, so this test failing to LINK means
// the dependency wiring in string-core/meson.build regressed.
TEST(OzzRuntime, LinksAndDefaultConstructs)
{
    ozz::animation::Skeleton skeleton;
    EXPECT_EQ(skeleton.num_joints(), 0);
    EXPECT_EQ(skeleton.num_soa_joints(), 0);
}
