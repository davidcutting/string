#pragma once

#include <memory>

#include <string/gpu/device.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/render_pass.hpp>

#include <volk.h>


namespace string::render
{

// The ui-dev sandbox background (brief 05, milestone 0): a fullscreen flat-design gradient drawn
// before the UI, standing in for the geometry pass's sky when the "ui" scene runs no geometry.
// Hot-reloadable Slang (ui_background.slang) via the registry, like UIPass.
class UIBackgroundPass final : public String::Pass
{
public:
    explicit UIBackgroundPass(String::engine_context& context);
    ~UIBackgroundPass() override;

    UIBackgroundPass(const UIBackgroundPass&) = delete;
    UIBackgroundPass& operator=(const UIBackgroundPass&) = delete;

    std::string_view debug_name() const override { return "ui_background"; }

    void update(float delta_time, uint16_t current_frame) override;
    void record(::string::gpu::command_recorder& recorder, uint16_t current_frame) override;

private:
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
