#pragma once

#include <filesystem>
#include <vector>

#include <string/core/tonemap.hpp>
#include <string/vulkan/render_pass.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/gpu/shader_program_registry.hpp>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <volk.h>

namespace String
{

// Fullscreen pass that samples one bindless texture (the offscreen HDR target), applies exposure
// and the brief-09 baked output-transform LUT (ACES 2.0-style DRT + HDR grading composed at bake
// time), and writes to the swapchain. Runtime cost: one 2D-strip LUT sample (manual trilinear).
class CompositePass final : public Pass
{
    string::gpu::device& device_;
    string::gpu::resource_allocator& allocator_;
    string::gpu::descriptor_table& descriptor_table_;
    // The pipeline is owned by the shader_program (hot-reloadable); the pass binds program_->current().
    string::gpu::shader_program* program_ = nullptr;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    uint32_t source_slot_ = 0;

    // Brief 09: the baked output-transform LUT (2D strip of lut_size_ slices; see tonemap.hpp for
    // the input-domain shaper). CPU copy kept for the capture writer (encode_display) so headless
    // captures encode the same transform the screen shows.
    string::gpu::resource_id lut_image_ = 0;
    uint32_t lut_slot_ = 0;
    uint32_t lut_size_ = 0;
    VkSampler lut_sampler_ = VK_NULL_HANDLE;
    std::vector<float> lut_cpu_;
    string::core::tonemap::Curve baked_curve_{};
    string::core::tonemap::Grading baked_grading_{};
    void bake_and_upload_lut(bool first);

public:
    CompositePass(string::gpu::device& device, string::gpu::resource_allocator& allocator,
                  string::gpu::descriptor_table& descriptor_table,
                  const std::filesystem::path& resources_path, VkFormat color_format,
                  string::gpu::shader_program_registry& registry);
    virtual ~CompositePass() override;

    // The renderer supplies the global set and the slot the HDR target is bound at
    // (re-supplied on resize, when the target is re-created and re-bound).
    void set_source(VkDescriptorSet descriptor_set, uint32_t source_slot);

    // The linear exposure scale the composite applies before its tonemap (EV100 CVar
    // r.exposure.ev100, or the auto-exposure value when r.exposure.auto is on, + the brief-07
    // kilo-unit factor). Exposed so the headless capture writer encodes the SAME image the
    // screen shows.
    static float exposure_scale();

    // Brief 09 auto-exposure: the post chain's histogram metering publishes its smoothed EV100
    // here each frame; exposure_scale() uses it while r.exposure.auto (STRING_EXPOSURE_AUTO) is
    // on. Before the first publish (warmup) the manual CVar value is used.
    static void set_auto_ev100(float ev100);
    static bool auto_exposure_enabled();

    // Calibration override: while active, exposure_scale() uses THIS EV100 and ignores both the
    // manual CVar and auto-exposure. The white-furnace test pins EV100 = log2(1000/1.2) (~9.70,
    // exposure scale exactly 1.0) so the radiance-1 furnace environment hits the tonemap at 1.0
    // and renders flat white — under scene exposure it reads as uniform grey instead, which
    // defeats the visual gate. Pass active = false to clear.
    static void set_exposure_override(float ev100, bool active);

    // Full display encode for the capture writer: exposure + grading + output transform via the
    // SAME baked LUT the composite samples (CPU trilinear). Returns display-linear [0,1].
    static glm::vec3 encode_display(glm::vec3 hdr);

    std::string_view debug_name() const override { return "composite"; }
    virtual void update(float delta_time, uint16_t current_frame) override;
    virtual void record(string::gpu::command_recorder& recorder, uint16_t current_frame) override;
};

}
