#pragma once

#include <vulkan/vulkan.h>
#include <memory>
#include <string/device.hpp>

namespace String
{
class PipelineGrid2D
{
public:
    explicit PipelineGrid2D(
        std::shared_ptr<Device>& device,
        const VkPushConstantRange& push_constant_range);
    ~PipelineGrid2D();

    VkPipeline get_pipeline() const;
    VkPipelineLayout get_pipeline_layout() const;
private:
    static constexpr char VERTEX_SHADER_PATH[] = "shaders/grid_2d_shader.vert.spv";
    static constexpr char FRAGMENT_SHADER_PATH[] = "shaders/grid_2d_shader.frag.spv";

    VkPipelineLayout pipeline_layout_;
    VkPipeline pipeline_;
    std::shared_ptr<Device> device_;
}; // class PipelineGrid2D
}  // namespace String
