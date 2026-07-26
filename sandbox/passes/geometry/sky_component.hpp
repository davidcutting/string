#pragma once

#include <string/gpu/shader_program.hpp>
#include <string/vulkan/pass_context.hpp>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <volk.h>

namespace sandbox
{

// Push constant for the procedural sky background (matches Push in shaders/sky_shader.slang; vec3s on
// 16-byte boundaries). Moved out of geometry_pass.hpp with the SkyComponent extraction (brief 11).
struct SkyPush
{
    glm::mat4 inv_view_proj;
    glm::vec3 camera_pos;  float furnace;   // brief 07: 1 -> uniform white background
    glm::vec3 sun_dir;     float _sp1;
    glm::vec3 sky_zenith;  float _sp2;
    glm::vec3 sky_ground;  float _sp3;           // ground ALBEDO (radiance derived in-shader)
    glm::vec3 sun_color;   float sun_intensity;  // klx perpendicular
};

// Per-frame inputs the sky draw reads from the shared scene state. The sun/sky colours are shared
// with shadows, IBL and SceneData, so they stay owned by GeometryPass and are passed in here.
struct SkyParams
{
    glm::mat4 view_proj;
    glm::vec3 camera_pos;
    glm::vec3 sun_dir;
    glm::vec3 sky_zenith;
    glm::vec3 sky_ground;
    glm::vec3 sun_color;
    float sun_intensity;
    bool furnace;
};

// Fullscreen procedural sky, drawn (in GeometryPass::record) before the geometry into the HDR target
// with no depth test/write, so opaque geometry overwrites it. Owns only its hot-reloadable pipeline;
// all lighting state is passed in. First component of the GeometryPass Phase-1 decomposition
// (brief 11) — establishes the component pattern.
class SkyComponent
{
    string::gpu::shader_program* program_ = nullptr;
public:
    SkyComponent() = default;
    // Registers sky_shader.slang through the hot-reload registry (recompiles + swaps on save).
    void init(String::PassContext& context);
    // Binds the current pipeline, pushes the sky/sun state, draws the fullscreen triangle.
    void record(VkCommandBuffer command_buffer, const SkyParams& params);
    // The pipeline the GeometryPass destructor tears down (kept there so teardown order is unchanged).
    string::gpu::shader_program* program() const { return program_; }
};

}  // namespace sandbox
