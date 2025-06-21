#pragma once

#include <vulkan/vulkan_core.h>
#include <memory>
#include <string/device.hpp>

namespace String
{

class PipelineLayoutBuilder
{
public:
    PipelineLayoutBuilder& set_descriptor_set_layout(const std::vector<VkDescriptorSetLayout>& descriptor_set_layouts);
    PipelineLayoutBuilder& set_push_constant_ranges(const std::vector<VkPushConstantRange>& push_constant_ranges);
    VkPipelineLayout build(const std::shared_ptr<Device>& device);
private:
    std::vector<VkDescriptorSetLayout> descriptor_set_layouts_;
    std::vector<VkPushConstantRange> push_constant_ranges_;
};

}