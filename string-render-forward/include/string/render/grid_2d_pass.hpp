#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/frame_graph.hpp>
#include <string/gpu/pass_context.hpp>
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

// Brief 20: a plain app-owned object. No base class — declare() authors it onto the app's graph and
// the recording callback captures `this`. The grid is static, so there is no per-frame CPU work.
class grid_2d_pass
{
    ::string::gpu::device& device_;
    VkPushConstantRange grid_2d_push_constant_range_;
    ::string::gpu::pipeline pipeline_;

public:
    // `samples` is the MSAA sample count of the colour/depth attachments this draws into: the
    // pipeline's rasterizationSamples must match the render-pass instance the graph opens.
    grid_2d_pass(string::engine_context& ctx, VkSampleCountFlagBits samples);
    ~grid_2d_pass();

    grid_2d_pass(const grid_2d_pass&) = delete;
    grid_2d_pass& operator=(const grid_2d_pass&) = delete;

    // Author onto the graph: a colour write into the scene target, and a read-only depth attachment
    // (the pipeline declares the D32 format but neither tests nor writes depth, so it never occludes
    // the geometry — it just has to share the geometry group's attachment set).
    void declare(::string::frame_graph& fg, ::string::gpu::image color, ::string::gpu::image depth);

private:
    void record(::string::pass_context& ctx);
};

}  // namespace string::render
