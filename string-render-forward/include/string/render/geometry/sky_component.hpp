#pragma once

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/pass_context.hpp>
#include <string/gpu/shader_program.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/frame_graph.hpp>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <volk.h>

namespace string::render
{

// Push constant for the procedural sky background (matches Push in shaders/sky_shader.slang; vec3s on
// 16-byte boundaries).
struct SkyPush
{
    glm::mat4 inv_view_proj;
    glm::vec3 camera_pos;  float furnace;   // brief 07: 1 -> uniform white background
    glm::vec3 sun_dir;     float _sp1;
    glm::vec3 sky_zenith;  float _sp2;
    glm::vec3 sky_ground;  float _sp3;           // ground ALBEDO (radiance derived in-shader)
    glm::vec3 sun_color;   float sun_intensity;  // klx perpendicular
};

// The per-frame CPU inputs the sky draw needs. The sun/sky colours are shared with shadows, IBL and
// SceneData, so they stay owned by the scene and are handed over in tick().
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

// Fullscreen procedural sky: a colour write into the scene target with no depth test/write, drawn
// before the opaque geometry (which overwrites it). Owns only its hot-reloadable pipeline.
//
// Brief 20: a plain app-owned object, no base class. declare() authors it onto the app's graph;
// tick() is ordinary per-frame CPU work that latches this frame's sun/sky/camera state. It touches
// no resource_id, no bindless slot and no barrier — the colour/depth attachments are logical handles
// the app declared, and load/store plus every transition derive from the declaration.
class sky_component
{
public:
    // `samples` is the MSAA sample count of the attachments this draws into (engine_context no longer
    // carries a sample count).
    sky_component(String::engine_context& ctx, VkSampleCountFlagBits samples);
    ~sky_component();

    sky_component(const sky_component&) = delete;
    sky_component& operator=(const sky_component&) = delete;

    // Author onto the graph: a colour write into the scene target, sharing the depth attachment
    // read-only (the pipeline declares the depth format but neither tests nor writes it), so the sky
    // joins the geometry render group rather than opening one of its own.
    //
    // `enabled` is the in-graph conditional the owner composes (the r.pass.sky CVar, and the
    // "no scene, no sky" early-out that used to sit at the top of record()). Empty => always on.
    void declare(::string::frame_graph& fg, ::string::gpu::image color, ::string::gpu::image depth,
                 ::string::enable_fn enabled = {});

    // Ordinary app code: latch the sun/sky/camera state this frame's draw will push.
    void tick(const SkyParams& params);

private:
    void record(::string::pass_context& ctx);

    ::string::gpu::device& device_;
    ::string::gpu::shader_program* program_ = nullptr;
    SkyParams params_{};
};

}  // namespace string::render
