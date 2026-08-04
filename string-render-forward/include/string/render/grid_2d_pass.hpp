#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include <string/vulkan/render_pass.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/device.hpp>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <volk.h>

namespace string::render
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
    ::string::gpu::device& device_;
    VkPushConstantRange grid_2d_push_constant_range_;
    ::string::gpu::pipeline pipeline_;

public:
    explicit Grid2DPass(String::engine_context& context);
    virtual ~Grid2DPass() override;

    // Stable identity for tooling (Tracy zones, inspector). Brief 06.
    std::string_view debug_name() const override { return "grid2d"; }

    // No update() override — the grid is static, so it uses Pass's default no-op.
    virtual void record(::string::gpu::command_recorder& recorder, uint16_t current_frame) override;
};

}  // namespace string::render