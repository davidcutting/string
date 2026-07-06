#pragma once

#include <volk.h>

#include <string/vulkan/device.hpp>

namespace String
{
class Pipeline3D
{
public:
    explicit Pipeline3D(
        const std::filesystem::path& resources_path,
        Device& device,
        const VkDescriptorSetLayout& descriptor_set_layouts,
        const VkPushConstantRange& push_constant_range);
    ~Pipeline3D();

    VkPipeline get_pipeline() const;
    VkPipelineLayout get_pipeline_layout() const;
private:
    static constexpr char VERTEX_SHADER_PATH[] = "shaders/3d_shader.vert.spv";
    static constexpr char FRAGMENT_SHADER_PATH[] = "shaders/3d_shader.frag.spv";

    VkPipelineLayout pipeline_layout_;
    VkPipeline pipeline_;
    Device& device_;
}; // class Pipeline3D
}  // namespace String
