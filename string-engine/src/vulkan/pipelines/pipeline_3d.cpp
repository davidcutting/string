#include <string/vulkan/pipelines/pipeline_3d.hpp>
#include <string/vulkan/pipeline_builder.hpp>
#include <string/vulkan/render_data.hpp>
#include <vector>

#include <volk.h>

namespace String
{

Pipeline3D::Pipeline3D(
    const std::filesystem::path& resources_path,
    Device& device,
    const VkDescriptorSetLayout& descriptor_set_layout)
: device_(device)
{
    std::vector<VkDescriptorSetLayout> descriptor_set_layouts = { descriptor_set_layout };
    pipeline_layout_ = PipelineLayoutBuilder()
        .set_descriptor_set_layout(descriptor_set_layouts)
        .build(device_);

    auto binding_description = Vertex::getBindingDescription();
    auto attribute_descriptions = Vertex::getAttributeDescriptions();

    const auto vert_relative_path = std::filesystem::path(VERTEX_SHADER_PATH);
    const auto frag_relative_path = std::filesystem::path(FRAGMENT_SHADER_PATH);

    pipeline_ = PipelineBuilder(device_)
        .add_vertex_shader(resources_path / vert_relative_path)
        .add_fragment_shader(resources_path / frag_relative_path)
        .set_vertex_binding(binding_description, attribute_descriptions)
        .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .set_tessellation()
        .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT)
        .set_multisampling()
        .enable_depth_stencil()
        .enable_color_blending()
        .build_graphics_pipeline(pipeline_layout_);
}

Pipeline3D::~Pipeline3D()
{
    vkDestroyPipeline(device_.get_device(), pipeline_, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), pipeline_layout_, nullptr);
}

VkPipeline Pipeline3D::get_pipeline() const
{
    return pipeline_;
}

VkPipelineLayout Pipeline3D::get_pipeline_layout() const
{
    return pipeline_layout_;
}

}
