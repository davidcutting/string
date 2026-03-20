#pragma once

#include <volk.h>

#include <filesystem>
#include <string/vulkan/device.hpp>

namespace String
{
class Pipeline2D
{
public:
    explicit Pipeline2D(
        const std::filesystem::path& resources_path,
        Device& device,
        const VkDescriptorSetLayout& descriptor_set_layout,
        const VkPushConstantRange& push_constant_range);
    ~Pipeline2D();

    VkPipeline get_pipeline() const;
    VkPipelineLayout get_pipeline_layout() const;
private:
    static constexpr char VERTEX_SHADER_PATH[] = "shaders/ui_shader.vert.spv";
    static constexpr char FRAGMENT_SHADER_PATH[] = "shaders/ui_shader.frag.spv";

    VkPipelineLayout pipeline_layout_;
    VkPipeline pipeline_;
    Device& device_;
}; // class Pipeline2D
}  // namespace String
