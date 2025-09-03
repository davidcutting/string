#include <filesystem>
#include <string/vulkan/device.hpp>
#include <string/vulkan/pipelines/hello_slang_pipeline.hpp>
#include <string/vulkan/pipeline_builder.hpp>
#include <string/vulkan/render_data.hpp>

#include <volk.h>

namespace String
{

HelloSlangPipeline::HelloSlangPipeline(
    const std::filesystem::path& resources_path,
    std::shared_ptr<Device>& device,
    const VkDescriptorSetLayout& descriptor_set_layout)
: device_(device)
{
    std::vector<VkDescriptorSetLayout> descriptor_set_layouts{descriptor_set_layout};
    std::vector<VkPushConstantRange> push_constant_ranges{};

    pipeline_layout_ = PipelineLayoutBuilder()
        .set_descriptor_set_layout(descriptor_set_layouts)
        .set_push_constant_ranges(push_constant_ranges)
        .build(device_);

    const auto relative_path = std::filesystem::path(COMPUTE_SHADER_PATH);

    pipeline_ = PipelineBuilder(device_)
        .add_compute_shader(resources_path / relative_path)
        .build_compute_pipeline(pipeline_layout_);
}

HelloSlangPipeline::~HelloSlangPipeline()
{
    vkDestroyPipeline(device_->get_device(), pipeline_, nullptr);
    vkDestroyPipelineLayout(device_->get_device(), pipeline_layout_, nullptr);
}

VkPipeline HelloSlangPipeline::get_pipeline() const
{
    return pipeline_;
}

VkPipelineLayout HelloSlangPipeline::get_pipeline_layout() const
{
    return pipeline_layout_;
}

}
