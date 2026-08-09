#include <string/render/post_pass.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

#include <string/core/cvar.hpp>
#include <string/core/logger.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/vulkan/passes/composite_pass.hpp>

#include <string/render/render_cvars.hpp>

namespace string::render
{

namespace
{

// Histogram log2-luminance domain (scene KILO-nit units, brief 07): 2^-10 .. 2^9 knits covers
// moonlit shadow (~1 cd/m^2) through 512 knits (well past sun-lit diffuse; the clamped sun disc
// saturates the top bin, which the percentile trim discards anyway).
constexpr float kLogMin = -10.0f;
constexpr float kLogMax = 9.0f;

// CPU mirror of ExposureOut in post.slang.
struct ExposureOut
{
    float avg_log_lum;
    uint32_t total;
    uint32_t kept;
    float _pad;
};

glm::uvec2 screen_of(const ::string::pass_context& ctx)
{
    return glm::uvec2(ctx.extent.width, ctx.extent.height);
}

// The bloom pyramid's base is half the viewport, and mip m is that halved m more times — the same
// arithmetic the pass has always used, now recomputed per record from the context's extent rather
// than cached at target-creation time. That is what lets a resize be a backing swap: the
// declarations, the pass count and the compiled plan are all unchanged.
glm::uvec2 bloom_base_of(const ::string::pass_context& ctx)
{
    return glm::max(screen_of(ctx) / 2u, glm::uvec2(1));
}

glm::uvec2 bloom_mip_size(const glm::uvec2& base, uint32_t m)
{
    return glm::uvec2(std::max(base.x >> m, 1u), std::max(base.y >> m, 1u));
}

}  // namespace

post_pass::post_pass(string::engine_context& ctx)
: device_(ctx.device)
, allocator_(ctx.allocator)
, descriptor_table_(ctx.descriptor_table)
, frames_in_flight_(ctx.frames_in_flight)
{
    // Register/touch the post CVars (central sandbox registration in debug_cvars handles the env
    // bridge; these accessors just make first use explicit here).
    cv_bloom_enabled();
    cv_bloom_intensity();
    cv_bloom_mips();

    // Per-slot host-visible exposure readback ring (16 B each; read frames_in_flight later). This
    // is a genuine CPU readback with a real temporal dependency, so it stays the pass's own ring
    // rather than becoming a graph resource — the graph orders GPU work, and nothing on the GPU
    // reads this back.
    readback_.resize(frames_in_flight_);
    readback_mapped_.resize(frames_in_flight_);
    for (uint16_t f = 0; f < frames_in_flight_; ++f)
    {
        readback_[f] = allocator_.create_resource(::string::gpu::buffer_info{
            .size = sizeof(ExposureOut),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                   | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_TO_CPU,
            .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                              | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        });
        readback_mapped_[f] = allocator_.get_buffer(readback_[f]).allocation_info.pMappedData;
        std::memset(readback_mapped_[f], 0, sizeof(ExposureOut));
    }

    // One post.slang, one compute pipeline per entry point (hot-reload registry).
    VkDescriptorSetLayout layout = descriptor_table_.get_layout();
    const auto make_entry = [&](const char* entry) {
        return ctx.shader_registry.create(
            ctx.resources_path / "shaders" / "post.slang",
            [layout, entry](::string::gpu::device& dev, const ::string::gpu::compiled_program& compiled) {
                ::string::gpu::pipeline p{};
                p.push_constants = compiled.layout.push_constant;
                p.pipeline_layout = ::string::gpu::pipeline_layout_builder()
                    .set_descriptor_set_layout({ layout })
                    .set_push_constant_ranges({ compiled.layout.push_constant })
                    .build(dev);
                ::string::gpu::pipeline_builder builder(dev, ::string::gpu::pipeline_type::COMPUTE);
                for (const auto& stage : compiled.stages)
                    if (stage.stage == VK_SHADER_STAGE_COMPUTE_BIT && stage.entry_point == entry)
                        builder.add_compute_shader_spirv(stage.spirv, stage.entry_point);
                p.pipeline = builder.build_compute_pipeline(p.pipeline_layout);
                p.pipeline_type = ::string::gpu::pipeline_type::COMPUTE;
                return p;
            });
    };
    hist_clear_program_ = make_entry("hist_clear_main");
    hist_program_ = make_entry("hist_main");
    hist_reduce_program_ = make_entry("hist_reduce_main");
    bloom_down_program_ = make_entry("bloom_down_main");
    bloom_up_program_ = make_entry("bloom_up_main");
    bloom_apply_program_ = make_entry("bloom_apply_main");
}

post_pass::~post_pass()
{
    const auto destroy_program = [&](::string::gpu::shader_program* prog) {
        if (!prog) return;
        const ::string::gpu::pipeline& p = prog->current();
        vkDestroyPipeline(device_.get_device(), p.pipeline, nullptr);
        vkDestroyPipelineLayout(device_.get_device(), p.pipeline_layout, nullptr);
    };
    destroy_program(hist_clear_program_);
    destroy_program(hist_program_);
    destroy_program(hist_reduce_program_);
    destroy_program(bloom_down_program_);
    destroy_program(bloom_up_program_);
    destroy_program(bloom_apply_program_);
    for (::string::gpu::resource_id id : readback_) allocator_.destroy_resource(id);
}

uint32_t post_pass::bloom_mip_count(VkExtent2D viewport)
{
    const glm::uvec2 base = glm::max(glm::uvec2(viewport.width, viewport.height) / 2u,
                                     glm::uvec2(1));
    uint32_t mips = uint32_t(std::clamp(cv_bloom_mips().get(), 1, 8));
    while (mips > 1 && (std::min(base.x, base.y) >> (mips - 1)) < 4) --mips;
    return mips;
}

// ================================================================================================
// authoring
// ================================================================================================

// One declaration per dispatch. Every edge the chain used to hold together with an unscoped global
// memory barrier is now a write->read (or write->write) relationship between two declarations over
// the same logical resource — the bins buffer for the histogram, and per-MIP slices of the bloom
// pyramid for the pyramid. The graph derives the ordering, the barrier scopes and the layouts.
void post_pass::declare(::string::frame_graph& fg, ::string::gpu::image hdr,
                        ::string::gpu::image bloom, uint32_t mips, ::string::gpu::buffer bins)
{
    bloom_mips_ = mips;

    // --- 1. Auto-exposure metering (histogram over the PRE-bloom resolved HDR) -------------------
    // clear -> accumulate -> reduce. The two barriers that used to separate them are these three
    // declarations of `bins`: WAW between clear and accumulate, RAW between accumulate and reduce.
    fg.pass("post.histogram.clear")
      .writes(bins)
      .compute([this, bins](::string::pass_context& ctx) { record_hist_clear(ctx, bins); });

    fg.pass("post.histogram.accumulate")
      .reads(hdr)
      .writes(bins)   // atomic read-modify-write; the storage-write scope covers both directions
      .compute([this, bins, hdr](::string::pass_context& ctx) { record_hist(ctx, bins, hdr); });

    fg.pass("post.histogram.reduce")
      .reads(bins)
      .compute([this, bins](::string::pass_context& ctx) { record_hist_reduce(ctx, bins); });

    // --- 2. Bloom (r.bloom.enabled; off = the identity/parity lever) -----------------------------
    // An in-graph conditional, not a recompile trigger: the passes stay declared and compiled and
    // are simply skipped for the frames the CVar is off.
    const auto bloom_on = [] { return cv_bloom_enabled().get(); };

    // Downsample pyramid: mip 0 reads the scene (Karis average + firefly clamp), mip m reads
    // mip m-1. Declaring the SLICES is what makes the chain derivable — mip m-1 and mip m no
    // longer collide as "the same image", so the graph sees mips-1 producer/consumer pairs where it
    // used to see one opaque pass with hand-rolled barriers inside it.
    for (uint32_t m = 0; m < mips; ++m)
    {
        ::string::pass_spec spec = fg.pass("post.bloom.down." + std::to_string(m));
        if (m == 0) spec.reads(hdr);
        else        spec.reads(bloom.mip(m - 1));
        spec.writes(bloom.mip(m))
            .toggle(bloom_on)
            .compute([this, hdr, bloom, m](::string::pass_context& ctx) {
                record_bloom_down(ctx, hdr, bloom, m);
            });
    }

    // Tent upsample-accumulate back to mip 0. Each step SAMPLES mip m+1 and read-modify-writes
    // mip m in the same dispatch (post.slang bloom_up_main: `cur + up` into dst_images). Only the
    // write is declared for mip m — access::storage_image_write's scope is WRITE|READ|SAMPLED and
    // its layout is GENERAL, so a read-modify-write is exactly what it already describes; declaring
    // a second, read use of the same slice would add nothing but a second entry to reason about.
    for (int m = int(mips) - 2; m >= 0; --m)
    {
        const auto mu = uint32_t(m);
        fg.pass("post.bloom.up." + std::to_string(mu))
          .reads(bloom.mip(mu + 1))
          .writes(bloom.mip(mu))
          .toggle(bloom_on)
          .compute([this, bloom, mu](::string::pass_context& ctx) {
              record_bloom_up(ctx, bloom, mu);
          });
    }

    // Apply into the scene target in place: sampled read of bloom mip 0, storage read-and-write of
    // the HDR colour. The write against hdr is what orders this after the histogram's and the first
    // downsample's sampled reads of the same image — the last of the six barriers.
    fg.pass("post.bloom.apply")
      .reads(bloom.mip(0))
      .writes(hdr)
      .toggle(bloom_on)
      .compute([this, hdr, bloom](::string::pass_context& ctx) {
          record_bloom_apply(ctx, hdr, bloom);
      });
}

// ================================================================================================
// per-frame CPU work
// ================================================================================================

void post_pass::tick(float dt)
{
    // The readback ring is this pass's own, so it is indexed by this pass's own frame counter: the
    // slot read here is the slot the reduce dispatch writes later this frame, and the value in it
    // was written frames_in_flight frames ago. The frame-slot timeline wait in begin_frame is what
    // makes reading it CPU-side safe.
    readback_slot_ = frames_in_flight_ ? uint32_t(frame_index_ % frames_in_flight_) : 0;

    if (frame_index_ >= frames_in_flight_ && readback_slot_ < readback_.size())
    {
        ExposureOut out{};
        std::memcpy(&out, readback_mapped_[readback_slot_], sizeof(out));
        if (cv_exposure_verify().get() && !verify_logged_ && out.total != 0)
        {
            verify_logged_ = true;
            STRING_LOG_INFO("[exposure] histogram total {} (expect {}x{} = {}), kept {} "
                            "(cut {:.2f}/{:.2f}), avg log2 lum {:.3f}",
                            out.total, extent_.width, extent_.height,
                            uint64_t(extent_.width) * extent_.height, out.kept,
                            cv_exposure_cut_low().get(), cv_exposure_cut_high().get(),
                            out.avg_log_lum);
        }
        if (out.total != 0)
        {
            // Scene luminance (knits) -> measured scene EV100: L_cd = 2^avg * 1000;
            // EV100 = log2(L_cd * 100 / 12.5) = log2(L_cd) + log2(8) with the saturation-based
            // convention. This is the "grey world" target that would render the metered average to
            // middle grey.
            const float measured_ev = out.avg_log_lum + std::log2(8000.0f);
            // Scene-referred exposure compensation (Frostbite/COD "Moving to PBR", Lagarde &
            // de Rousiers, Fig. 42): instead of always metering the average to middle grey (which
            // pumps a night scene up to daytime brightness — the user's complaint), bias the target
            // EV as a function of the MEASURED scene EV so dark scenes stay dark and bright scenes
            // stay bright, with a smooth key-value ramp in between. Piecewise-linear over EV:
            //   dark (<= key_lo): let ~half the underexposure stand (slope 0.5) -> night stays dim;
            //   mid (key_lo..key_hi): full grey-world metering (slope 1) around the reference key;
            //   bright (>= key_hi): compress highlights (slope 0.5) -> daylight doesn't blow out.
            // key_ev is the reference "middle grey renders at 0.18" scene EV (14.6 = sunny-16).
            const float key_ev = 14.6f;
            const float key_lo = key_ev - 3.0f;    // ~overcast/interior boundary
            const float key_hi = key_ev + 2.0f;    // ~direct-sun boundary
            float comp_ev;
            if (measured_ev < key_lo)
                comp_ev = key_ev + (measured_ev - key_ev) * 0.5f
                        - (key_ev - key_lo) * 0.5f;  // continuous at key_lo
            else if (measured_ev > key_hi)
                comp_ev = key_ev + (measured_ev - key_ev) * 0.5f
                        + (key_hi - key_ev) * 0.5f;  // continuous at key_hi
            else
                comp_ev = measured_ev;               // slope-1 grey-world in the mid band
            float target = comp_ev - cv_exposure_comp().get();
            target = std::clamp(target, cv_exposure_min_ev().get(), cv_exposure_max_ev().get());
            if (!ev_valid_)
            {
                ev100_ = target;
                ev_valid_ = true;
            }
            else
            {
                // Smoothed adaptation, separate rates: "up" = EV rising (scene got brighter,
                // eye stops down fast), "down" = EV falling (dark adaptation, slower).
                const float rate = target > ev100_ ? cv_exposure_speed_up().get()
                                                   : cv_exposure_speed_down().get();
                ev100_ += (target - ev100_) * (1.0f - std::exp(-dt * rate));
                // Snap once within a millistop: the exponential approach never exactly converges
                // in fp, which would leave a permanent last-ulp EV difference between two runs
                // whose warmup (texture-streaming timing) differed — the run-twice AE=0 gate
                // requires the settled state to be history-free.
                if (std::abs(target - ev100_) < 1e-3f) ev100_ = target;
            }
            string::composite_pass::set_auto_ev100(ev100_);
        }
    }
    ++frame_index_;
}

// ================================================================================================
// recording
// ================================================================================================

void post_pass::dispatch(::string::pass_context& ctx, ::string::gpu::shader_program* prog,
                         const PostPush& push, uint32_t gx, uint32_t gy)
{
    ::string::gpu::command_recorder& recorder = ctx.rec;
    const ::string::gpu::pipeline& p = prog->current();
    VkDescriptorSet set = descriptor_table_.get_set();
    recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    recorder.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline_layout,
                                  0, 1, &set, 0, nullptr);
    recorder.push_constants(p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(PostPush), &push);
    recorder.dispatch(gx, gy, 1);
}

PostPush post_pass::base_push(::string::pass_context& ctx, ::string::gpu::buffer bins) const
{
    PostPush base{};
    base.hist = ctx.address(bins);
    base.readback = allocator_.get_buffer(readback_[readback_slot_]).device_address;
    base.log_min = kLogMin;
    base.log_inv_range = 1.0f / (kLogMax - kLogMin);
    return base;
}

void post_pass::record_hist_clear(::string::pass_context& ctx, ::string::gpu::buffer bins)
{
    dispatch(ctx, hist_clear_program_, base_push(ctx, bins), 1, 1);
}

void post_pass::record_hist(::string::pass_context& ctx, ::string::gpu::buffer bins,
                            ::string::gpu::image hdr)
{
    extent_ = ctx.extent;
    const glm::uvec2 screen = screen_of(ctx);
    PostPush push = base_push(ctx, bins);
    push.src_slot = ctx.slot(hdr);
    push.src_size = screen;
    dispatch(ctx, hist_program_, push, (screen.x + 15) / 16, (screen.y + 15) / 16);
}

void post_pass::record_hist_reduce(::string::pass_context& ctx, ::string::gpu::buffer bins)
{
    PostPush push = base_push(ctx, bins);
    push.p0 = std::clamp(cv_exposure_cut_low().get(), 0.0f, 0.95f);
    push.p1 = std::clamp(cv_exposure_cut_high().get(), 0.0f, 0.5f);
    dispatch(ctx, hist_reduce_program_, push, 1, 1);
}

void post_pass::record_bloom_down(::string::pass_context& ctx, ::string::gpu::image hdr,
                                  ::string::gpu::image bloom, uint32_t m)
{
    const glm::uvec2 screen = screen_of(ctx);
    const glm::uvec2 base = bloom_base_of(ctx);
    const glm::uvec2 dst = bloom_mip_size(base, m);

    PostPush push{};
    push.log_min = kLogMin;
    push.log_inv_range = 1.0f / (kLogMax - kLogMin);
    if (m == 0)
    {
        push.src_slot = ctx.slot(hdr);
        push.src_size = screen;
        push.flags = 1u;   // Karis + clamp
        push.p0 = std::max(cv_bloom_clamp().get(), 0.0f);
    }
    else
    {
        // The source slot is now the SLICE's own texture slot, not the whole image's — so the
        // shader's source mip is selected by the descriptor rather than by the lod field, and the
        // lod bits (flags 8..15) are 0. Same texels, same taps, same dispatch; the mip selection
        // moved from a hand-packed push field to the declaration the graph already needed.
        push.src_slot = ctx.slot(bloom.mip(m - 1));
        push.src_size = bloom_mip_size(base, m - 1);
    }
    push.dst_slot = ctx.slot(bloom.mip(m));
    push.dst_size = dst;
    dispatch(ctx, bloom_down_program_, push, (dst.x + 7) / 8, (dst.y + 7) / 8);
}

void post_pass::record_bloom_up(::string::pass_context& ctx, ::string::gpu::image bloom, uint32_t m)
{
    const glm::uvec2 base = bloom_base_of(ctx);
    const glm::uvec2 dst = bloom_mip_size(base, m);

    PostPush push{};
    push.log_min = kLogMin;
    push.log_inv_range = 1.0f / (kLogMax - kLogMin);
    push.src_slot = ctx.slot(bloom.mip(m + 1));   // slice slot; lod bits stay 0 (see down)
    push.src_size = bloom_mip_size(base, m + 1);
    push.dst_slot = ctx.slot(bloom.mip(m));
    push.dst_size = dst;
    push.p0 = std::clamp(cv_bloom_radius().get(), 0.0f, 2.0f);
    dispatch(ctx, bloom_up_program_, push, (dst.x + 7) / 8, (dst.y + 7) / 8);
}

void post_pass::record_bloom_apply(::string::pass_context& ctx, ::string::gpu::image hdr,
                                   ::string::gpu::image bloom)
{
    const glm::uvec2 screen = screen_of(ctx);
    const glm::uvec2 base = bloom_base_of(ctx);

    PostPush push{};
    push.log_min = kLogMin;
    push.log_inv_range = 1.0f / (kLogMax - kLogMin);
    push.src_slot = ctx.slot(bloom.mip(0));   // bloom_apply_main samples at lod 0.0 unconditionally
    push.src_size = base;
    push.dst_slot = ctx.slot(hdr);
    push.dst_size = screen;
    push.p0 = std::max(cv_bloom_intensity().get(), 0.0f);
    dispatch(ctx, bloom_apply_program_, push, (screen.x + 7) / 8, (screen.y + 7) / 8);

    // --- Outline/rim slot (brief 10 decides the style; depth + reconstructed normals are
    // available at this point in the frame) ---
    record_outline_slot(ctx.rec);
}

}  // namespace string::render
