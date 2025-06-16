#pragma once

#include <vulkan/vulkan.h>
#include <memory>
#include <string/device.hpp>

namespace String
{
class HelloSlangPipeline
{
public:
    explicit HelloSlangPipeline(
        const std::filesystem::path& resources_path,
        std::shared_ptr<Device>& device,
        const VkDescriptorSetLayout& descriptor_set_layout);
    ~HelloSlangPipeline();

    VkPipeline get_pipeline() const;
    VkPipelineLayout get_pipeline_layout() const;
private:
    static constexpr char COMPUTE_SHADER_PATH[] = "shaders/slang/test_compute_shader.comp.slang.spv";

    VkPipelineLayout pipeline_layout_;
    VkPipeline pipeline_;
    std::shared_ptr<Device> device_;
}; // class PipelineGrid2D
}  // namespace String
