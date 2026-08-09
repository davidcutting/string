#pragma once

#include <filesystem>
#include <vector>

#include <string/core/tonemap.hpp>
#include <string/vulkan/lens.hpp>
#include <string/vulkan/frame_graph.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/gpu/pass_context.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/gpu/shader_program_registry.hpp>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <volk.h>

namespace string
{

// Fullscreen pass that samples one bindless texture (the offscreen HDR target), applies exposure
// and the brief-09 baked output-transform LUT (ACES 2.0-style DRT + HDR grading composed at bake
// time), and writes to the swapchain. Runtime cost: one 2D-strip LUT sample (manual trilinear).
//
// Brief 20 — the reference shape for every pass in the engine:
//   * NO base class. This is a plain object the application owns; the graph never sees the type.
//   * It owns its own GPU state and nothing else's.
//   * declare() authors it onto the app's frame_graph, capturing `this`. The declaration lives next
//     to the state that knows what it touches, and the app decides whether and when to call it.
//   * The recording callback resolves everything it needs through pass_context. NOTHING is pushed
//     in from outside — no set_source(), no bind_color_source(), no slot handed over at init.
//   * tick() is ordinary per-frame CPU work the app calls. It is not a graph concept.
class composite_pass
{
    string::gpu::device& device_;
    string::gpu::resource_allocator& allocator_;
    // Uploads the baked LUT through the frame's declared uploads pass rather than its own submit.
    string::TransferBatch& transfer_;
    string::gpu::descriptor_table& descriptor_table_;
    // The pipeline is owned by the shader_program (hot-reloadable); the pass binds program_->current().
    string::gpu::shader_program* program_ = nullptr;

    // Brief 09: the baked output-transform LUT (2D strip of lut_size_ slices; see tonemap.hpp for
    // the input-domain shaper). CPU copy kept for the capture writer (encode_display) so headless
    // captures encode the same transform the screen shows.
    string::gpu::resource_id lut_image_ = 0;
    uint32_t lut_slot_ = 0;
    uint32_t lut_size_ = 0;
    std::vector<float> lut_cpu_;
    string::core::tonemap::Curve baked_curve_{};
    string::core::tonemap::Grading baked_grading_{};
    void bake_and_upload_lut(bool first);

public:
    composite_pass(string::engine_context& ctx, VkFormat color_format);
    ~composite_pass();

    // Author this pass onto the graph. `hdr` is the resolved scene colour it samples; `target` is
    // what it writes (the swapchain). Both are logical handles the app declared — the pass resolves
    // their bindless slots through pass_context while it records, never before.
    void declare(string::frame_graph& fg, string::gpu::image hdr, string::gpu::image target);

    // The linear exposure scale the composite applies before its tonemap (EV100 CVar
    // r.exposure.ev100, or the auto-exposure value when r.exposure.auto is on, + the brief-07
    // kilo-unit factor). Exposed so the headless capture writer encodes the SAME image the
    // screen shows.
    static float exposure_scale();

    // Brief 09 auto-exposure: the post chain's histogram metering publishes its smoothed EV100
    // here each frame; exposure_scale() uses it while r.exposure.auto (STRING_EXPOSURE_AUTO) is
    // on. Before the first publish (warmup) the manual CVar value is used.
    static void set_auto_ev100(float ev100);

    // Calibration override: while active, exposure_scale() uses THIS EV100 and ignores both the
    // manual CVar and auto-exposure. The white-furnace test pins EV100 = log2(1000/1.2) (~9.70,
    // exposure scale exactly 1.0) so the radiance-1 furnace environment hits the tonemap at 1.0
    // and renders flat white — under scene exposure it reads as uniform grey instead, which
    // defeats the visual gate. Pass active = false to clear.
    static void set_exposure_override(float ev100, bool active);

    // Full display encode for the capture writer: exposure + grading + output transform via the
    // SAME baked LUT the composite samples (CPU trilinear). Returns display-linear [0,1].
    static glm::vec3 encode_display(glm::vec3 hdr);

    // Per-frame CPU work: re-bake the output-transform LUT when a grading/tonemap CVar changed.
    // Ordinary app code, called before the graph executes.
    void tick();

private:
    // The recording callback, bound in declare(). Resolves `hdr` through the context.
    void record(string::pass_context& ctx, string::gpu::image hdr);
};

}
