#include "grid_2d_pass.hpp"

#include <string/gpu/pipeline_builder.hpp>

namespace sandbox
{
using namespace String;

Grid2DPass::Grid2DPass(PassContext& context)
: device_(context.device)
{
    grid_2d_push_constant_range_ = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(Grid2DParams)
    };

    pipeline_.pipeline_layout = string::gpu::pipeline_layout_builder()
        .set_descriptor_set_layout({})
        .set_push_constant_ranges({ grid_2d_push_constant_range_ })
        .build(device_);

    // 2D background grid: procedural (no vertex input), no depth test/write (but declares the
    // offscreen D32 format so the pipeline matches the pass) so it never occludes the geometry.
    pipeline_.pipeline = string::gpu::pipeline_builder(device_)
        .add_vertex_shader(context.resources_path / "shaders/grid_2d_shader.vert.spv")
        .add_fragment_shader(context.resources_path / "shaders/grid_2d_shader.frag.spv")
        .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .set_tessellation()
        .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .set_multisampling(context.sample_count)
        .enable_depth_stencil(false, false)
        .enable_color_blending()
        .build_graphics_pipeline(pipeline_.pipeline_layout);
    pipeline_.pipeline_type = string::gpu::pipeline_type::GRAPHICS;

    // Draws into the offscreen color target (background; no depth).
    usages = {
        { context.color_target, Access::ColorWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT },
    };
}

Grid2DPass::~Grid2DPass()
{
    vkDestroyPipeline(device_.get_device(), pipeline_.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), pipeline_.pipeline_layout, nullptr);
}

void Grid2DPass::record(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    (void)current_frame;
    VkCommandBuffer& command_buffer = recorder.get_command_buffer();

    Grid2DParams params = {
        .background_color = {0.0f, 0.0f, 0.0f, 0.0f},
        .grid_color = {0.4f, 0.4f, 0.4f, 1.0f},
        .border_color = {0.8f, 0.8f, 0.8f, 1.0f},
        .axis_color = {0.6f, 0.6f, 0.6f, 1.0f},
        .grid_resolution = {50.0f, 50.0f},
        .grid_center = { screen_size.width / 2, screen_size.height / 2 },
        .grid_size = { 800.0f, 800.0f },
        .screen_size = { screen_size.width, screen_size.height },
        .line_width = 1.0f,
        .fade_distance = 500.0f,
        .border_width = 3.0f,
        .axis_width = 2.0f,
        .show_border = 1.0f,
        .show_axes = 1.0f
    };

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.pipeline);
    vkCmdPushConstants(
        command_buffer,
        pipeline_.pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(params),
        &params
    );
    vkCmdDraw(command_buffer, 3, 1, 0, 0);
}

}
