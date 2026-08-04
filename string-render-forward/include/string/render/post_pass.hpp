#pragma once

#include <cstdint>
#include <vector>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/vulkan/render_pass.hpp>
#include <string/vulkan/engine_context.hpp>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <volk.h>

namespace string::render
{

// Push for the brief-09 post chain (matches Push in shaders/post.slang; std430 push layout —
// pointers 8-aligned at 0/8, uint2s 8-aligned at 24/32).
struct PostPush
{
    VkDeviceAddress hist;      // 0   histogram bins (256 x uint, FrameScratch region)
    VkDeviceAddress readback;  // 8   {avg_log_lum, total, kept, pad} (host-visible ring)
    uint32_t src_slot;         // 16
    uint32_t dst_slot;         // 20
    glm::uvec2 src_size;       // 24
    glm::uvec2 dst_size;       // 32
    uint32_t flags;            // 40  bit0 = first downsample (Karis+clamp); bits 8..15 = src lod
    float p0;                  // 44
    float p1;                  // 48
    float log_min;             // 52
    float log_inv_range;       // 56
    uint32_t _pad;             // 60
};
static_assert(offsetof(PostPush, src_slot) == 16);
static_assert(offsetof(PostPush, src_size) == 24);
static_assert(offsetof(PostPush, dst_size) == 32);
static_assert(offsetof(PostPush, flags) == 40);
static_assert(sizeof(PostPush) == 64);

// Brief 09: the post-processing chain — a compute_only frame pass that runs OUTSIDE any rendering
// group, between the scene's MSAA resolve into the HDR target and the composite (the renderer
// places it from its toposorted position; barriers derive from the declared usages).
//
//   1. Luminance histogram over the resolved HDR target (256 bins, FrameScratch transient) +
//      percentile-trimmed reduce into a per-slot host-visible ring; update() reads the value
//      frames_in_flight later, smooths EV100 (separate up/down rates, min/max clamps) and
//      publishes it to CompositePass (r.exposure.auto gates consumption).
//   2. Karis bloom: threshold-free 13-tap downsample pyramid (Karis average + firefly clamp on
//      the first mip) + tent upsample-accumulate, applied back INTO the HDR target in place so
//      composite + capture writer both see it. Tight defaults per the locked "no haze" look.
//   3. Outline/rim SLOT (brief 10 decides the style): record_outline_slot() is the no-op hook —
//      it runs after bloom with the scene depth (geometry's hz.depth chain) and depth-
//      reconstructed normals available; nothing is dispatched today.
class PostProcessPass final : public String::Pass
{
    ::string::gpu::device& device_;
    ::string::gpu::resource_allocator& allocator_;
    ::string::gpu::descriptor_table& descriptor_table_;
    ::string::gpu::FrameScratch* scratch_ = nullptr;
    uint16_t frames_in_flight_ = 1;

    // One post.slang, one pipeline per entry point (hot-reload registry).
    ::string::gpu::shader_program* hist_clear_program_ = nullptr;
    ::string::gpu::shader_program* hist_program_ = nullptr;
    ::string::gpu::shader_program* hist_reduce_program_ = nullptr;
    ::string::gpu::shader_program* bloom_down_program_ = nullptr;
    ::string::gpu::shader_program* bloom_up_program_ = nullptr;
    ::string::gpu::shader_program* bloom_apply_program_ = nullptr;

    // The resolved HDR target (renderer-owned; re-supplied via bind_color_source on resize).
    uint32_t color_sampled_slot_ = 0;
    ::string::gpu::resource_id color_id_ = 0;
    uint32_t color_storage_slot_ = UINT32_MAX;

    // Bloom mip chain (half-res base). Lives permanently in GENERAL (07 cubemap pattern).
    ::string::gpu::resource_id bloom_image_ = 0;
    uint32_t bloom_sampled_slot_ = 0;
    std::vector<VkImageView> bloom_mip_views_;
    std::vector<uint32_t> bloom_mip_slots_;
    uint32_t bloom_mips_ = 0;
    glm::uvec2 bloom_base_{ 0, 0 };
    glm::uvec2 bloom_screen_{ 0, 0 };
    bool bloom_layout_init_ = false;
    VkSampler sampler_ = VK_NULL_HANDLE;
    void ensure_targets();
    void destroy_targets();

    // Auto-exposure state.
    VkDeviceSize hist_off_ = 0;                          // FrameScratch region (256 x uint)
    std::vector<::string::gpu::resource_id> readback_;     // per-slot host-visible ExposureOut
    std::vector<void*> readback_mapped_;
    float ev100_ = 14.6f;
    bool ev_valid_ = false;
    uint64_t frame_index_ = 0;
    bool verify_logged_ = false;

public:
    explicit PostProcessPass(String::engine_context& context);
    virtual ~PostProcessPass() override;

    std::string_view debug_name() const override { return "post"; }
    // Nature (compute-only) declared fluently by the app when authoring the graph (brief 11 endgame).

    void bind_color_source(uint32_t sampled_slot, ::string::gpu::resource_id physical_id) override;

    virtual void update(float delta_time, uint16_t current_frame) override;
    virtual void record(::string::gpu::command_recorder& recorder, uint16_t current_frame) override;

    // Brief 09 outline/rim slot: depth + reconstructed normals are available here (hz.depth of
    // this frame's slot, post phase-2). Intentionally a no-op until the brief-10 style decision.
    void record_outline_slot(VkCommandBuffer) {}
};

}  // namespace string::render
