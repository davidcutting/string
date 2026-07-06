#include <string/vulkan/passes/grid_2d_pass.hpp>

namespace String
{

Grid2DPass::Grid2DPass(Device& device, const std::filesystem::path& resources_path, const uint16_t& frames_in_flight)
: device_(device)
{
    (void)frames_in_flight;
    grid_2d_push_constant_range_ = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(Grid2DParams)
    };
    pipeline_grid_2d_ = std::make_unique<PipelineGrid2D>(resources_path, device_, grid_2d_push_constant_range_);
}

Grid2DPass::~Grid2DPass()
{
    pipeline_grid_2d_.reset();
}

void Grid2DPass::update(const float& delta_time, const uint16_t& current_frame)
{
    (void)delta_time;
    (void)current_frame;
}

void Grid2DPass::record(CommandRecorder& recorder, const uint16_t& current_frame)
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

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_grid_2d_->get_pipeline());
    vkCmdPushConstants(
        command_buffer,
        pipeline_grid_2d_->get_pipeline_layout(),
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(params),
        &params
    );
    vkCmdDraw(command_buffer, 3, 1, 0, 0);
}

}