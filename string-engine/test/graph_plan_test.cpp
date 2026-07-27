#include <gtest/gtest.h>

#include <string/vulkan/graph_plan.hpp>

using namespace String;

namespace
{
constexpr string::gpu::resource_id kDepth = 1;
constexpr string::gpu::resource_id kColor = 2;
constexpr string::gpu::resource_id kGrid = 3;
}

// Authored order that is already a valid topological order must be preserved verbatim — the
// renderer's byte-parity gates rely on stable scheduling (brief 04e M2).
TEST(RenderGraphTest, StableToposortPreservesAuthoredOrder)
{
    GraphPlan graph = PlanBuilder()
        .add_pass("depth")
            .use(kDepth, Access::DepthWrite, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT)
            .end_pass()
        .add_pass("cull")
            .use(kDepth, Access::DepthRead, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
            .use(kGrid, Access::StorageWrite, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
            .end_pass()
        .add_pass("shade")
            .use(kGrid, Access::StorageRead, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT)
            .use(kColor, Access::ColorWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)
            .end_pass()
        .build();

    ASSERT_EQ(graph.toposorted.size(), 3u);
    EXPECT_EQ(graph.toposorted, (std::vector<uint32_t>{ 0, 1, 2 }));
}

// Independent passes keep declaration order (smallest-index-first tie break).
TEST(RenderGraphTest, IndependentPassesKeepDeclarationOrder)
{
    GraphPlan graph = PlanBuilder()
        .add_pass("a").use(kDepth, Access::DepthWrite, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT).end_pass()
        .add_pass("b").use(kColor, Access::ColorWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT).end_pass()
        .add_pass("c").use(kGrid, Access::StorageWrite, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT).end_pass()
        .build();

    EXPECT_EQ(graph.toposorted, (std::vector<uint32_t>{ 0, 1, 2 }));
}

// Read-then-write on the same resource in declaration order is a WAR edge: the reader must come
// before the later writer (the 04d ghosting class — a later pass clobbering what an earlier one
// still reads must be ordered, not accidental).
TEST(RenderGraphTest, WarEdgeOrdersReaderBeforeLaterWriter)
{
    GraphPlan graph = PlanBuilder()
        .add_pass("writer1").use(kColor, Access::ColorWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT).end_pass()
        .add_pass("reader").use(kColor, Access::SampledRead, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT).end_pass()
        .add_pass("writer2").use(kColor, Access::ColorWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT).end_pass()
        .build();

    // reader -> writer2 edge exists.
    const auto& adj = graph.adjacency[1];
    EXPECT_NE(std::find(adj.begin(), adj.end(), 2u), adj.end());
    EXPECT_EQ(graph.toposorted, (std::vector<uint32_t>{ 0, 1, 2 }));
}

// One pass declaring multiple writes of the SAME resource (e.g. a stats buffer written from its
// compute stage AND its draw stages) is a single writer — it must not forge a self-edge/"cycle".
TEST(RenderGraphTest, MultipleWritesFromOnePassAreNotACycle)
{
    GraphPlan graph = PlanBuilder()
        .add_pass("geometry")
            .use(kGrid, Access::StorageWrite, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
            .use(kGrid, Access::StorageWrite, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT)
            .use(kGrid, Access::StorageRead, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
            .end_pass()
        .add_pass("composite")
            .use(kColor, Access::ColorWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)
            .end_pass()
        .build();
    EXPECT_EQ(graph.toposorted, (std::vector<uint32_t>{ 0, 1 }));
}

TEST(RenderGraphTest, LifetimesSpanFirstToLastUse)
{
    GraphPlan graph = PlanBuilder()
        .add_pass("depth").use(kDepth, Access::DepthWrite, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT).end_pass()
        .add_pass("mid").use(kColor, Access::ColorWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT).end_pass()
        .add_pass("late").use(kDepth, Access::DepthRead, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT).end_pass()
        .build();

    const ResourceLifetime& depth = graph.resource_lifetimes.at(kDepth);
    EXPECT_EQ(depth.first.value(), 0u);
    EXPECT_EQ(depth.last.value(), 2u);
    EXPECT_EQ(depth.first_writer.value(), 0u);
}
