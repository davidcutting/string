#pragma once

#include <filesystem>

#include <string/gpu/device.hpp>
#include <string/gpu/pass_context.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/frame_graph.hpp>

#include <volk.h>

namespace string::render
{

// Brief 18 M4 — a Shadertoy scene: one fullscreen pass running a USER's Slang shader, with the
// standard inputs and nothing else. No geometry, no assets. The point is the iteration loop, not
// the feature list: save the file, see the result, and never lose the session to a typo.
//
// The REPL half is not implemented here — `shader_program_registry` already recompiles on save,
// keeps the last good pipeline when a compile fails, records the diagnostic for the error overlay,
// and swaps at a frame boundary. This pass is a client of that.
//
// ONE DELIBERATE DIFFERENCE from every other pass: a broken shader AT STARTUP is survivable. The
// registry throws for engine shaders, because a broken engine shader is a build error — but a user
// shader that does not compile yet is the same not-yet-valid state as a broken save, so the scene
// loads with the error overlay showing and recovers on the next save. `valid()` is false until then
// and record() draws nothing.
//
// POST IS BYPASSED (user decision, brief 18): a shader scene declares no post chain, so there is no
// exposure, bloom or GTAO between the shader and the screen — exactly as the `ui` scene works.
//
// PARTIAL, and worth knowing before porting a Shadertoy: the renderer's COMPOSITE still runs, and
// composite tonemaps (ACES) + encodes sRGB. So what a shader writes here is scene-referred linear,
// like ui_background.slang, NOT the display-referred 0..1 a Shadertoy shader emits. Bypassing
// composite as well is the remaining piece of the brief's "what the shader writes is what you see" —
// composite is authored by the renderer rather than the scene, so it is not a scene-side switch.
//
// Brief 20: a plain app-owned object. declare() authors it onto the app's graph; tick() is ordinary
// per-frame CPU work the app calls (it advances iTime/iFrame), not a graph concept.
class shadertoy_pass
{
public:
    // `samples` is the MSAA sample count of the attachments this draws into.
    shadertoy_pass(string::engine_context& ctx, std::filesystem::path shader,
                   VkSampleCountFlagBits samples);
    ~shadertoy_pass();

    shadertoy_pass(const shadertoy_pass&) = delete;
    shadertoy_pass& operator=(const shadertoy_pass&) = delete;

    // Author onto the graph: a colour write into the scene target, sharing the depth attachment
    // read-only (the pipeline declares the D32 format but neither tests nor writes depth).
    void declare(::string::frame_graph& fg, ::string::gpu::image color, ::string::gpu::image depth);

    // Advance the Shadertoy clock. Ordinary app code, called before the graph executes.
    void tick(float dt);

    // False when the user's shader has never compiled successfully (so there is no pipeline to
    // bind). Recovers on the next successful hot reload.
    [[nodiscard]] bool valid() const noexcept { return program_ != nullptr; }

private:
    void record(::string::pass_context& ctx);

    // The Shadertoy input set, by the names people expect. Kept to one push-constant block so a
    // shader needs no descriptors at all — the whole surface is `[[vk::push_constant]]`.
    struct Push
    {
        float resolution[2];   // iResolution: pixels
        float time;            // iTime: seconds since the scene loaded
        float time_delta;      // iTimeDelta: seconds
        float mouse[4];        // iMouse: xy = current, zw = click position
        int   frame;           // iFrame
        float _pad[3];
    };

    ::string::gpu::device& device_;
    ::string::gpu::shader_program* program_ = nullptr;
    std::filesystem::path shader_;
    float time_ = 0.0f;
    float dt_ = 0.0f;
    int frame_ = 0;
};

}  // namespace string::render
