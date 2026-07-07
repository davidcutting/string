#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include <string/vulkan/render_pass.hpp>
#include <string/vulkan/pass_context.hpp>
#include <string/vulkan/pipeline.hpp>
#include <string/vulkan/device.hpp>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <volk.h>

namespace sandbox
{

struct Grid2DParams {
    glm::vec4 background_color;
    glm::vec4 grid_color;
    glm::vec4 border_color;
    glm::vec4 axis_color;
    glm::vec2 grid_resolution;
    glm::vec2 grid_center;
    glm::vec2 grid_size;
    glm::vec2 screen_size;
    float line_width;
    float fade_distance;
    float border_width;
    float axis_width;
    float show_border;
    float show_axes;
};

class Grid2DPass final : public String::Pass
{
    String::Device& device_;
    VkPushConstantRange grid_2d_push_constant_range_;
    String::Pipeline pipeline_;

public:
    explicit Grid2DPass(String::PassContext& context);
    virtual ~Grid2DPass() override;

    // No update() override — the grid is static, so it uses Pass's default no-op.
    virtual void record(String::CommandRecorder& recorder, uint16_t current_frame) override;
};

}  // namespace sandbox