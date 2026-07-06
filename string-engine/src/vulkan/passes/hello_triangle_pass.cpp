#include <string/vulkan/passes/hello_triangle_pass.hpp>
#include <string/vulkan/pipeline_builder.hpp>

namespace String
{

HelloTrianglePass::HelloTrianglePass(Device& device, const std::filesystem::path& resources_path, const uint16_t& frames_in_flight)
: device_(device)
{
    (void)frames_in_flight;

    // The triangle is generated entirely from gl_VertexIndex, so there are no vertex
    // buffers, descriptors, or push constants — an empty pipeline layout suffices.
    pipeline.pipeline_layout = PipelineLayoutBuilder().build(device_);

    pipeline.pipeline = PipelineBuilder(device_)
        .add_vertex_shader(resources_path / "shaders/hello_triangle.vert.spv")
        .add_fragment_shader(resources_path / "shaders/hello_triangle.frag.spv")
        .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .set_tessellation()
        .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .set_multisampling()
        .enable_depth_stencil()
        .enable_color_blending()
        .build_graphics_pipeline(pipeline.pipeline_layout);

    pipeline.pipeline_type = PipelineType::GRAPHICS;
}

HelloTrianglePass::~HelloTrianglePass()
{
    vkDestroyPipeline(device_.get_device(), pipeline.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), pipeline.pipeline_layout, nullptr);
}

void HelloTrianglePass::update(const float& delta_time, const uint16_t& current_frame)
{
    (void)delta_time;
    (void)current_frame;
}

void HelloTrianglePass::record(CommandRecorder& recorder, const uint16_t& current_frame)
{
    (void)current_frame;
    VkCommandBuffer command_buffer = recorder.get_command_buffer();
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
    vkCmdDraw(command_buffer, 3, 1, 0, 0);
}

}
