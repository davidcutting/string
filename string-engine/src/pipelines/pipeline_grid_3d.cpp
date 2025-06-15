#include <vulkan/vulkan.h>
#include <string/device.hpp>
#include <string/pipelines/pipeline_grid_3d.hpp>
#include <string/render_data.hpp>
#include <string/pipeline_builder.hpp>
#include <string/render_data.hpp>

namespace String
{

PipelineGrid3D::PipelineGrid3D(
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

    auto binding_description = Vertex::getBindingDescription();
    auto attribute_descriptions = Vertex::getAttributeDescriptions();

    pipeline_ = PipelineBuilder(device_)
        .add_vertex_shader(VERTEX_SHADER_PATH)
        .add_fragment_shader(FRAGMENT_SHADER_PATH)
        .set_vertex_binding(binding_description, attribute_descriptions)
        .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .set_tessellation()
        .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT)
        .set_multisampling()
        .enable_depth_stencil()
        .enable_color_blending()
        .build_graphics_pipeline(pipeline_layout_);
}

PipelineGrid3D::~PipelineGrid3D()
{
    vkDestroyPipeline(device_->get_device(), pipeline_, nullptr);
    vkDestroyPipelineLayout(device_->get_device(), pipeline_layout_, nullptr);
}

VkPipeline PipelineGrid3D::get_pipeline() const
{
    return pipeline_;
}

VkPipelineLayout PipelineGrid3D::get_pipeline_layout() const
{
    return pipeline_layout_;
}

}
