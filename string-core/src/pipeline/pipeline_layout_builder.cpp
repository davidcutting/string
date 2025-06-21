#include <string/pipeline/pipeline_layout_builder.hpp>

#include <vulkan/vulkan_core.h>
#include <cstdint>
#include <memory>

namespace String
{

PipelineLayoutBuilder& PipelineLayoutBuilder::set_descriptor_set_layout(const std::vector<VkDescriptorSetLayout>& descriptor_set_layouts)
{
    descriptor_set_layouts_ = descriptor_set_layouts;
    return *this;
}

PipelineLayoutBuilder& PipelineLayoutBuilder::set_push_constant_ranges(const std::vector<VkPushConstantRange>& push_constant_ranges)
{
    push_constant_ranges_ = push_constant_ranges;
    return *this;
}

VkPipelineLayout PipelineLayoutBuilder::build(const std::shared_ptr<Device>& device)
{
    VkPipelineLayout pipeline_layout;

    // clang-format off
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = static_cast<uint32_t>(descriptor_set_layouts_.size()),
        .pSetLayouts = descriptor_set_layouts_.data(),
        .pushConstantRangeCount = static_cast<uint32_t>(push_constant_ranges_.size()),
        .pPushConstantRanges = push_constant_ranges_.data()
    };
    // clang-format on

    if (vkCreatePipelineLayout(device->get_device(), &pipeline_layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create pipeline layout!");
    }

    return pipeline_layout;
}

}