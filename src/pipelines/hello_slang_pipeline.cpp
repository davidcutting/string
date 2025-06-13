#include <vulkan/vulkan.h>
#include <string/device.hpp>
#include <string/pipelines/hello_slang_pipeline.hpp>
#include <string/pipeline_builder.hpp>
#include <string/render_data.hpp>

namespace String
{

HelloSlangPipeline::HelloSlangPipeline(
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

    pipeline_ = PipelineBuilder(device_)
        .add_compute_shader(COMPUTE_SHADER_PATH)
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
