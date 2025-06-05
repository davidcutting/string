#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>
#include <memory>
#include <string/device.hpp>

namespace String
{
class Pipeline3D
{
public:
    explicit Pipeline3D(
        std::shared_ptr<Device>& device,
        const VkDescriptorSetLayout& descriptor_set_layouts);
    ~Pipeline3D();

    VkPipeline get_pipeline() const;
    VkPipelineLayout get_pipeline_layout() const;
private:
    static constexpr char VERTEX_SHADER_PATH[] = "shaders/3d_shader.vert.spv";
    static constexpr char FRAGMENT_SHADER_PATH[] = "shaders/3d_shader.frag.spv";

    VkPipelineLayout pipeline_layout_;
    VkPipeline pipeline_;
    std::shared_ptr<Device> device_;
}; // class Pipeline3D
}  // namespace String