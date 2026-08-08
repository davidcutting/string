#include <gtest/gtest.h>

#include <string/vulkan/frame_graph.hpp>

using namespace string;

// These tests exercise the parts of the graph that are pure: subresource collision, stage resolution
// and the conditional/degrade rules. Ordering, allocation and barrier emission all need a device and
// are covered by the headless render gates instead.
//
// The old suite's "known limitation" tests for order-dependent RAW/WAR are gone with the limitation:
// declarations now carry subresource ranges, so a mip chain does not self-collide.

namespace
{

// A pass_spec's declarations, read back for assertion. The graph exposes them through
// declarations(); nothing else in the engine reads a pass's uses directly.
const pass_decl& only_pass(const frame_graph& fg)
{
    EXPECT_EQ(fg.declarations().size(), 1u);
    return fg.declarations().front();
}

}  // namespace

// --- stage resolution ---------------------------------------------------------------------------

// The defect this guards: a read declared BEFORE the terminal .compute() used to stamp the raster
// default, because the pass kind was not yet known. Every compute pass in the engine got
// FRAGMENT_SHADER barriers.
TEST(FrameGraphStages, ComputePassGetsComputeStage)
{
    frame_graph fg;
    const gpu::image a = fg.image({ .name = "a" });
    const gpu::image b = fg.image({ .name = "b" });
    fg.pass("c").reads(a).writes(b).compute([](pass_context&) {});

    const pass_decl& p = only_pass(fg);
    for (const resource_use& u : p.uses)
        EXPECT_EQ(u.stage, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

TEST(FrameGraphStages, RasterPassGetsFragmentStage)
{
    frame_graph fg;
    const gpu::image a = fg.image({ .name = "a" });
    fg.pass("r").reads(a).raster([](pass_context&) {});
    EXPECT_EQ(only_pass(fg).uses.front().stage, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
}

// An access that can only happen at one stage implies it, so pass code needs no mask.
TEST(FrameGraphStages, SingleStageAccessImpliesItsStage)
{
    frame_graph fg;
    const gpu::buffer buf = fg.buffer({ .name = "b" });
    fg.pass("draw").reads(buf, access::indirect_read).raster([](pass_context&) {});
    EXPECT_EQ(only_pass(fg).uses.front().stage, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);
}

TEST(FrameGraphStages, ExplicitStageIsPreserved)
{
    frame_graph fg;
    const gpu::buffer buf = fg.buffer({ .name = "b" });
    fg.pass("m").reads(buf, access::storage_read, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
                .raster([](pass_context&) {});
    EXPECT_EQ(only_pass(fg).uses.front().stage, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT);
}

// --- subresource identity -----------------------------------------------------------------------

// The property the whole mip-chain design rests on: adjacent mips are distinct, overlapping ranges
// are not. Without this a HiZ or bloom chain would serialise against itself.
TEST(FrameGraphSlices, AdjacentMipsAreDistinct)
{
    frame_graph fg;
    const gpu::image img = fg.image({ .name = "pyramid", .mip_levels = 4 });
    EXPECT_FALSE(img.mip(0) == img.mip(1));
    EXPECT_TRUE(img.mip(2) == img.mip(2));
}

TEST(FrameGraphSlices, WholeImageIsNotAMipSlice)
{
    frame_graph fg;
    const gpu::image img = fg.image({ .name = "i", .mip_levels = 4 });
    EXPECT_TRUE(img.whole().whole_image());
    EXPECT_FALSE(img.mip(0).whole_image());
    EXPECT_FALSE(img.whole() == img.mip(0));
}

TEST(FrameGraphSlices, LayersAreDistinct)
{
    frame_graph fg;
    const gpu::image cube = fg.image({ .name = "cube", .array_layers = 6, .cube = true });
    EXPECT_FALSE(cube.layer(0) == cube.layer(1));
}

// --- conditionals -------------------------------------------------------------------------------

// A toggle is re-evaluated live and does not invalidate anything: flipping it must not require a
// recompile, which is the property the whole authored-once seam rests on.
TEST(FrameGraphConditionals, ToggleIsReadLive)
{
    frame_graph fg;
    bool on = true;
    const gpu::image a = fg.image({ .name = "a" });
    fg.pass("p").writes(a).toggle([&on] { return on; }).compute([](pass_context&) {});

    EXPECT_TRUE(fg.passes().front().enabled);
    on = false;
    EXPECT_FALSE(fg.passes().front().enabled);
}

// Introspection lists every AUTHORED pass, including disabled ones — tooling must be able to show a
// pass that is currently off.
TEST(FrameGraphConditionals, DisabledPassesStillEnumerate)
{
    frame_graph fg;
    const gpu::image a = fg.image({ .name = "a" });
    fg.pass("on").writes(a).compute([](pass_context&) {});
    fg.pass("off").writes(a).toggle([] { return false; }).compute([](pass_context&) {});

    const std::vector<pass_info> passes = fg.passes();
    ASSERT_EQ(passes.size(), 2u);
    EXPECT_TRUE(passes[0].enabled);
    EXPECT_FALSE(passes[1].enabled);
}

// --- declaration bookkeeping --------------------------------------------------------------------

TEST(FrameGraphDeclarations, RequiresIsMarkedDistinctlyFromReads)
{
    frame_graph fg;
    const gpu::image a = fg.image({ .name = "a" });
    const gpu::image b = fg.image({ .name = "b" });
    fg.pass("p").reads(a).requires_(b).compute([](pass_context&) {});

    const pass_decl& p = only_pass(fg);
    ASSERT_EQ(p.uses.size(), 2u);
    ASSERT_EQ(p.required.size(), 2u);
    EXPECT_FALSE(p.required[0]);
    EXPECT_TRUE(p.required[1]);
}

TEST(FrameGraphDeclarations, AttachmentsCarryTheirOwnStages)
{
    frame_graph fg;
    const gpu::image color = fg.image({ .name = "c" });
    const gpu::image depth = fg.image({ .name = "d" });
    fg.pass("g").color(color).depth(depth).raster([](pass_context&) {});

    const pass_decl& p = only_pass(fg);
    EXPECT_EQ(p.uses[0].how, access::color_write);
    EXPECT_EQ(p.uses[0].stage, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    EXPECT_EQ(p.uses[1].how, access::depth_write);
    // BOTH depth stages: the loadOp clear and early-Z land at EARLY_FRAGMENT_TESTS, the late-Z write
    // and the MIN depth resolve complete at LATE. Naming only EARLY left every later reader's
    // derived barrier missing the late write (sync validation: WAW against vkCmdEndRendering).
    EXPECT_EQ(p.uses[1].stage, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                               | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
}

TEST(FrameGraphDeclarations, LaneIsDeclaredNotInferred)
{
    frame_graph fg;
    const gpu::buffer b = fg.buffer({ .name = "b" });
    fg.pass("froxel").writes(b).async().compute([](pass_context&) {});
    EXPECT_EQ(only_pass(fg).lane, pass_lane::async);
    EXPECT_EQ(fg.passes().front().lane, pass_lane::async);
}
