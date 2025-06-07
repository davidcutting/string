#pragma once

#include <vulkan/vulkan.h>
#include <memory>
#include <string/device.hpp>

namespace String
{
class PipelineGrid3D
{
public:
    explicit PipelineGrid3D(
        std::shared_ptr<Device>& device,
        const VkDescriptorSetLayout& descriptor_set_layout,
        const VkPushConstantRange& push_constant_range);
    ~PipelineGrid3D();

    VkPipeline get_pipeline() const;
    VkPipelineLayout get_pipeline_layout() const;
private:
    static constexpr char VERTEX_SHADER_PATH[] = "shaders/grid_3d_shader.vert.spv";
    static constexpr char FRAGMENT_SHADER_PATH[] = "shaders/grid_3d_shader.frag.spv";

    VkPipelineLayout pipeline_layout_;
    VkPipeline pipeline_;
    std::shared_ptr<Device> device_;
}; // class PipelineGrid3D
}  // namespace String
