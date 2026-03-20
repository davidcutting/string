#pragma once

#include <volk.h>

#include <string/vulkan/device.hpp>

namespace String
{
class PipelineGrid3D
{
public:
    explicit PipelineGrid3D(
        Device& device,
        const VkDescriptorSetLayout& descriptor_set_layout,
        const VkPushConstantRange& push_constant_range);
    ~PipelineGrid3D();

    VkPipeline get_pipeline() const;
    VkPipelineLayout get_pipeline_layout() const;
private:
    static constexpr char VERTEX_SHADER_PATH[] = "shaders/grid_3d_shader.vert.spv";
    static constexpr char FRAGMENT_SHADER_PATH[] = "shaders/grid_3d_shader.frag.spv";
    std::string vertex_shader_path;
    std::string fragment_shader_path;

    VkPipelineLayout pipeline_layout_;
    VkPipeline pipeline_;
    Device& device_;
}; // class PipelineGrid3D
}  // namespace String
