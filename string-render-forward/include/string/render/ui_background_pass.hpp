#pragma once

#include <memory>

#include <string/gpu/device.hpp>
#include <string/gpu/pass_context.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/frame_graph.hpp>

#include <volk.h>


namespace string::render
{

// The ui-dev sandbox background (brief 05, milestone 0): a fullscreen flat-design gradient drawn
// before the UI, standing in for the geometry pass's sky when the "ui" scene runs no geometry.
// Hot-reloadable Slang (ui_background.slang) via the registry, like the UI pass.
//
// Brief 20: a plain app-owned object. declare() authors it onto the app's graph; tick() advances the
// gradient's clock and is ordinary app code, not a graph concept.
class ui_background_pass
{
public:
    // `samples` is the MSAA sample count of the attachments this draws into.
    ui_background_pass(String::engine_context& ctx, VkSampleCountFlagBits samples);
    ~ui_background_pass();

    ui_background_pass(const ui_background_pass&) = delete;
    ui_background_pass& operator=(const ui_background_pass&) = delete;

    // Author onto the graph: a colour write into the scene target, sharing the depth attachment
    // read-only (the pipeline declares the D32 format but neither tests nor writes depth).
    void declare(::string::frame_graph& fg, ::string::gpu::image color, ::string::gpu::image depth);

    void tick(float dt);

private:
    void record(::string::pass_context& ctx);

    struct Push
    {
        float screen_size[2];
        float time;
        float _pad;
    };

    ::string::gpu::device& device_;
    ::string::gpu::shader_program* program_ = nullptr;
    float time_ = 0.0f;
};

}  // namespace string::render
