#include <vulkan/vulkan.h>
#include <string/device.hpp>
#include <string/pipelines/pipeline_grid_2d.hpp>
#include <string/pipeline_builder.hpp>
#include <string/render_data.hpp>

namespace String
{

PipelineGrid2D::PipelineGrid2D(
    std::shared_ptr<Device>& device,
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

    pipeline_ = PipelineBuilder(device_)
        .add_vertex_shader(VERTEX_SHADER_PATH)
        .add_fragment_shader(FRAGMENT_SHADER_PATH)
        .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .set_tessellation()
        .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .set_multisampling()
        .enable_color_blending()
        .build(pipeline_layout_);
}

PipelineGrid2D::~PipelineGrid2D()
{
    vkDestroyPipeline(device_->get_device(), pipeline_, nullptr);
    vkDestroyPipelineLayout(device_->get_device(), pipeline_layout_, nullptr);
}

VkPipeline PipelineGrid2D::get_pipeline() const
{
    return pipeline_;
}

VkPipelineLayout PipelineGrid2D::get_pipeline_layout() const
{
    return pipeline_layout_;
}

}
