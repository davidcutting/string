#include <string/vulkan/device.hpp>
#include <string/vulkan/pipelines/pipeline_2d.hpp>
#include <string/vulkan/pipeline_builder.hpp>
#include <string/vulkan/render_data.hpp>

#include <volk.h>

namespace String
{

Pipeline2D::Pipeline2D(
    const std::filesystem::path& resources_path,
    Device& device,
    const VkDescriptorSetLayout& descriptor_set_layout,
    const VkPushConstantRange& push_constant_range)
: device_(device)
{
    std::vector<VkDescriptorSetLayout> descriptor_set_layouts{ descriptor_set_layout };
    std::vector<VkPushConstantRange> push_constant_ranges{ push_constant_range };

    pipeline_layout_ = PipelineLayoutBuilder()
        .set_descriptor_set_layout(descriptor_set_layouts)
        .set_push_constant_ranges(push_constant_ranges)
        .build(device_);

    const auto vert_relative_path = std::filesystem::path(VERTEX_SHADER_PATH);
    const auto frag_relative_path = std::filesystem::path(FRAGMENT_SHADER_PATH);

    pipeline_ = PipelineBuilder(device_)
        .add_vertex_shader(resources_path / vert_relative_path)
        .add_fragment_shader(resources_path / frag_relative_path)
        .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .set_tessellation()
        .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .set_multisampling()
        .enable_color_blending()
        .build_graphics_pipeline(pipeline_layout_);
}

Pipeline2D::~Pipeline2D()
{
    vkDestroyPipeline(device_.get_device(), pipeline_, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), pipeline_layout_, nullptr);
}

VkPipeline Pipeline2D::get_pipeline() const
{
    return pipeline_;
}

VkPipelineLayout Pipeline2D::get_pipeline_layout() const
{
    return pipeline_layout_;
}

}
