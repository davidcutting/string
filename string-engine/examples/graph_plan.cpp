// Render-graph planner example. The implementation lives in the library now
// (src/vulkan/render_graph.cpp, brief 04e M2) — this example only demonstrates the API.
#include <string/vulkan/graph_plan.hpp>

#include <print>

using namespace String;

int main() {
    // Fake resource IDs for now
    string::gpu::resource_id depth_id   = 1;
    string::gpu::resource_id color_id   = 2;
    string::gpu::resource_id lightgrid_id = 3;

    GraphPlan graph = PlanBuilder()
        .add_pass("depth_prepass")
            .use(depth_id, Access::DepthWrite, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT)
            .end_pass()
        .add_pass("light_cull")
            .use(depth_id, Access::DepthRead, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
            .use(lightgrid_id, Access::StorageWrite, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
            .end_pass()
        .add_pass("forward_shading")
            .use(depth_id, Access::DepthRead, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT)
            .use(lightgrid_id, Access::StorageRead, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT)
            .use(color_id, Access::ColorWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)
            .end_pass()
        .build();

    for (auto& pass : graph.passes)
    {
        std::println("PassNode: {}", pass.name);
        for (auto& usage : pass.usages)
            std::println("  {} {} ({})", is_write(usage.access) ? "Writes" : "Reads",
                usage.resource, (int) usage.access);
    }

    std::println("Execution order:");
    for (int idx : graph.toposorted)
        std::println("  {}", graph.passes[idx].name);

    return 0;
}
