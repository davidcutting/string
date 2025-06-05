#pragma once

#include <vulkan/vulkan.h>
#include <memory>
#include <string/device.hpp>

namespace String
{
class Pipeline2D
{
public:
    explicit Pipeline2D(
        std::shared_ptr<Device>& device,
        const std::vector<VkDescriptorSetLayout>& descriptor_set_layouts,
        const std::vector<VkPushConstantRange>& push_constant_ranges);
    ~Pipeline2D();

    VkPipeline get_pipeline() const;
private:
    static constexpr char VERTEX_SHADER_PATH[] = "shaders/ui_shader.vert.spv";
    static constexpr char FRAGMENT_SHADER_PATH[] = "shaders/ui_shader.frag.spv";

    VkPipelineLayout pipeline_layout_;
    VkPipeline pipeline_;
    std::shared_ptr<Device> device_;
}; // class Pipeline2D
}  // namespace String