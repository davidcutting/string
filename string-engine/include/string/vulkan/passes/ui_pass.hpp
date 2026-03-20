#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include <string/vulkan/allocator.hpp>
#include <string/vulkan/render_pass.hpp>
#include <string/vulkan/pipelines/pipeline_2d.hpp>
#include <string/vulkan/device.hpp>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <volk.h>

namespace String
{

struct UIShaderConfig {
    glm::vec2 screen_size;
    glm::uint num_shapes;
    float delta_time;
};

struct UIElement {
    glm::vec4 fill;
    glm::vec4 stroke;
    glm::vec2 position;
    float radius;
    float stroke_width;
};

class UIPass final : public Pass
{
    Device& device_;
    std::unique_ptr<Pipeline2D> pipeline_2d_;

    UIShaderConfig ui_push_constant_;
    std::vector<std::unique_ptr<Buffer>> ui_shapes_ssbo_;
    std::vector<std::span<UIElement>> ui_elements_mapped_;

public:
    UIPass(Device& device, const std::filesystem::path& resources_path, const uint16_t& frames_in_flight);
    virtual ~UIPass() override;

    virtual void update(const float& delta_time, const uint16_t& current_frame) override;
    virtual void record(CommandRecorder& recorder, const uint16_t& current_frame) override;
};

}