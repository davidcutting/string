#pragma once

#include <memory>

#include <string/gpu/device.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/render_pass.hpp>

#include <volk.h>

#include "ui_scene.hpp"

namespace sandbox
{

// The ui-dev sandbox background (brief 05, milestone 0): a fullscreen flat-design gradient drawn
// before the UI, standing in for the geometry pass's sky when the "ui" scene runs no geometry. Also
// owns the UI-dev orbit camera: it advances a UiSceneDriver each frame and writes the projection +
// synthetic anchors into the shared UiScene the UI author reads for world-anchored nameplates/pings.
// Hot-reloadable Slang (ui_background.slang) via the registry, like UIPass.
class UIBackgroundPass final : public String::Pass
{
public:
    UIBackgroundPass(String::engine_context& context, std::shared_ptr<UiScene> scene,
                     std::uint32_t anchor_count);
    ~UIBackgroundPass() override;

    UIBackgroundPass(const UIBackgroundPass&) = delete;
    UIBackgroundPass& operator=(const UIBackgroundPass&) = delete;

    std::string_view debug_name() const override { return "ui_background"; }

    void update(float delta_time, uint16_t current_frame) override;
    void record(string::gpu::command_recorder& recorder, uint16_t current_frame) override;

private:
    struct Push
    {
        float screen_size[2];
        float time;
        float _pad;
    };

    string::gpu::device& device_;
    std::shared_ptr<UiScene> scene_;
    UiSceneDriver driver_;
    string::gpu::shader_program* program_ = nullptr;
    float time_ = 0.0f;
};

}  // namespace sandbox
