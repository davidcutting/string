#pragma once

#include <cstdint>
#include <vector>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/pass_context.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/resource.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/vulkan/frame_graph.hpp>
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
    VkDeviceAddress hist;      // 0   histogram bins (256 x uint, graph transient buffer)
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

// Brief 09: the post-processing chain — luminance-histogram auto-exposure metering plus the Karis
// bloom pyramid, running on the resolved HDR target between the scene and the composite.
//
// Brief 20 shape (see composite_pass, the reference): NO base class. This is a plain object the
// application owns; the graph never sees the type. declare() authors it onto the app's frame_graph
// and every recording callback resolves what it needs through pass_context — nothing is pushed in
// from outside, so bind_color_source() and its three hand-held slots are gone.
//
// The chain that used to be ONE record hook with six unscoped global VkMemoryBarrier2s is now
// (3 + mips + (mips-1) + 1) declared compute passes. Every one of those barriers is a declared
// write->read edge now: the histogram's three stages over the bins buffer, and the bloom pyramid's
// per-MIP chain, expressible because `gpu::image::mip(n)` slices are first-class and the graph
// tracks state per sub-resource.
//
//   1. Luminance histogram over the resolved HDR target (256 bins in a graph transient buffer) +
//      percentile-trimmed reduce into a per-slot host-visible ring; tick() reads the value
//      frames_in_flight later, smooths EV100 (separate up/down rates, min/max clamps) and
//      publishes it to composite_pass (r.exposure.auto gates consumption).
//   2. Karis bloom: threshold-free 13-tap downsample pyramid (Karis average + firefly clamp on
//      the first mip) + tent upsample-accumulate, applied back INTO the HDR target in place so
//      composite + capture writer both see it.
//   3. Outline/rim SLOT (brief 10 decides the style): record_outline_slot() is the no-op hook.
class post_pass
{
    ::string::gpu::device& device_;
    ::string::gpu::resource_allocator& allocator_;
    ::string::gpu::descriptor_table& descriptor_table_;
    uint16_t frames_in_flight_ = 1;

    // One post.slang, one pipeline per entry point (hot-reload registry).
    ::string::gpu::shader_program* hist_clear_program_ = nullptr;
    ::string::gpu::shader_program* hist_program_ = nullptr;
    ::string::gpu::shader_program* hist_reduce_program_ = nullptr;
    ::string::gpu::shader_program* bloom_down_program_ = nullptr;
    ::string::gpu::shader_program* bloom_up_program_ = nullptr;
    ::string::gpu::shader_program* bloom_apply_program_ = nullptr;

    // Mip count of the bloom transient, as the APP declared it. Fixed at author time because the
    // graph is authored once: one declared pass per mip, and a resize only re-sizes the backing.
    uint32_t bloom_mips_ = 0;

    // Auto-exposure state. readback_ is the one genuine temporal dependency in this pass: a
    // 16-byte GPU_TO_CPU buffer per frame-in-flight, WRITTEN by the reduce dispatch and READ by
    // tick() frames_in_flight frames later. It stays the pass's own ring, indexed by the pass's
    // own frame counter so the write and the read cannot disagree about the slot.
    std::vector<::string::gpu::resource_id> readback_;
    std::vector<void*> readback_mapped_;
    uint32_t readback_slot_ = 0;
    float ev100_ = 14.6f;
    bool ev_valid_ = false;
    uint64_t frame_index_ = 0;
    bool verify_logged_ = false;
    // Last recorded viewport, kept only so the one-shot r.exposure.verify log can state the pixel
    // count the histogram total is expected to match.
    VkExtent2D extent_{ 0, 0 };

    void dispatch(::string::pass_context& ctx, ::string::gpu::shader_program* prog,
                  const PostPush& push, uint32_t gx, uint32_t gy);
    PostPush base_push(::string::pass_context& ctx, ::string::gpu::buffer bins) const;

    // The recording callbacks, bound in declare().
    void record_hist_clear(::string::pass_context& ctx, ::string::gpu::buffer bins);
    void record_hist(::string::pass_context& ctx, ::string::gpu::buffer bins,
                     ::string::gpu::image hdr);
    void record_hist_reduce(::string::pass_context& ctx, ::string::gpu::buffer bins);
    void record_bloom_down(::string::pass_context& ctx, ::string::gpu::image hdr,
                           ::string::gpu::image bloom, uint32_t m);
    void record_bloom_up(::string::pass_context& ctx, ::string::gpu::image bloom, uint32_t m);
    void record_bloom_apply(::string::pass_context& ctx, ::string::gpu::image hdr,
                            ::string::gpu::image bloom);

public:
    explicit post_pass(string::engine_context& ctx);
    ~post_pass();

    // The bloom pyramid's mip count for a given viewport: r.bloom.mips clamped to [1,8], then
    // shrunk while the smallest dimension of the half-res base would drop below 4 texels at the
    // deepest mip. The APP calls this to fill transient_image_info::mip_levels, and hands the same
    // number to declare() — one number, one rule, one place.
    static uint32_t bloom_mip_count(VkExtent2D viewport);

    // Author this pass onto the graph.
    //   hdr    — the resolved scene colour. Sampled by the histogram and by the first bloom
    //            downsample; storage-written IN PLACE by the bloom apply.
    //   bloom  — the half-res mip pyramid, an APP-DECLARED graph transient
    //            (viewport_scaled = true, viewport_scale = 0.5f). It used to be an image this pass
    //            allocated itself with a hand-built VkImageView + a synthetic storage slot per mip;
    //            ctx.slot(bloom.mip(m)) replaces both.
    //   mips   — bloom's declared mip count (bloom_mip_count()).
    //   bins   — the 256-uint histogram accumulator, an app-declared transient buffer. It carries
    //            the RAW edges that used to be two of the six global memory barriers.
    void declare(::string::frame_graph& fg, ::string::gpu::image hdr, ::string::gpu::image bloom,
                 uint32_t mips, ::string::gpu::buffer bins);

    // Per-frame CPU work: consume this slot's metering readback, smooth EV100 and publish it.
    // Ordinary app code, called before the graph executes. Not a graph concept.
    void tick(float dt);

    // Brief 09 outline/rim slot: depth + reconstructed normals are available here. Intentionally a
    // no-op until the brief-10 style decision.
    void record_outline_slot(VkCommandBuffer) {}
};

}  // namespace string::render
