#include <gtest/gtest.h>

#include <string/vulkan/frame_scratch.hpp>

using string::gpu::FrameScratch;
using string::gpu::ScratchLifetime;

// Two regions whose lifetimes DO overlap (both default whole-frame) must be placed disjoint — this
// is the byte-identical old-bump-allocator behaviour the app relies on today.
TEST(FrameScratchTest, OverlappingLifetimesPackDisjoint)
{
    FrameScratch s;
    const VkDeviceSize a = s.reserve(256, 256);   // default whole-frame lifetime
    const VkDeviceSize b = s.reserve(256, 256);   // default whole-frame lifetime -> overlaps a
    EXPECT_EQ(a, 0u);
    EXPECT_EQ(b, 256u);          // placed after a, not aliased
    EXPECT_EQ(s.size(), 512u);
}

// Two regions whose lifetimes do NOT overlap alias the same memory (greedy interval-packing) — the
// arena's total size stays at one region, not two.
TEST(FrameScratchTest, DisjointLifetimesAliasMemory)
{
    FrameScratch s;
    const VkDeviceSize a = s.reserve(256, 256, ScratchLifetime{ 0, 1 });   // live only in pass 0
    const VkDeviceSize b = s.reserve(256, 256, ScratchLifetime{ 1, 2 });   // live only in pass 1
    EXPECT_EQ(a, 0u);
    EXPECT_EQ(b, 0u);            // reuses a's bytes — lifetimes don't overlap
    EXPECT_EQ(s.size(), 256u);   // total = one region, memory saved
}

// A region live across BOTH passes cannot alias either of two disjoint earlier regions; greedy
// first-fit places it past the highest colliding one.
TEST(FrameScratchTest, SpanningLifetimeCannotAlias)
{
    FrameScratch s;
    s.reserve(256, 256, ScratchLifetime{ 0, 1 });                  // pass 0 -> offset 0
    s.reserve(256, 256, ScratchLifetime{ 1, 2 });                  // pass 1 -> aliases at offset 0
    const VkDeviceSize c = s.reserve(256, 256, ScratchLifetime{ 0, 2 });  // spans both -> cannot alias
    EXPECT_EQ(c, 256u);
    EXPECT_EQ(s.size(), 512u);
}
