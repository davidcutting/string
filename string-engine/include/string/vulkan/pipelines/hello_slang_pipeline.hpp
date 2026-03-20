#pragma once

#include <volk.h>

#include <string/vulkan/device.hpp>

namespace String
{
class HelloSlangPipeline
{
public:
    explicit HelloSlangPipeline(
        const std::filesystem::path& resources_path,
        Device& device,
        const VkDescriptorSetLayout& descriptor_set_layout);
    ~HelloSlangPipeline();

    VkPipeline get_pipeline() const;
    VkPipelineLayout get_pipeline_layout() const;
private:
    static constexpr char COMPUTE_SHADER_PATH[] = "shaders/slang/test_compute_shader.comp.slang.spv";

    VkPipelineLayout pipeline_layout_;
    VkPipeline pipeline_;
    Device& device_;
}; // class PipelineGrid2D
}  // namespace String
