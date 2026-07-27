#include "post_pass.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <string/core/cvar.hpp>
#include <string/core/logger.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/vulkan/passes/composite_pass.hpp>
#include <string/vulkan/vulkan_utils.hpp>

#include "../debug_cvars.hpp"

namespace sandbox
{

namespace vku = String::vku;

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

}  // namespace

PostProcessPass::PostProcessPass(String::engine_context& context)
: device_(context.device)
, allocator_(context.allocator)
, descriptor_table_(context.descriptor_table)
, scratch_(&context.scratch)
, frames_in_flight_(context.frames_in_flight)
{
    // Register/touch the post CVars (central sandbox registration in debug_cvars handles the env
    // bridge; these accessors just make first use explicit here).
    cv_bloom_enabled();
    cv_bloom_intensity();

    // Linear clamp sampler for the bloom chain (bilinear tent taps; must not wrap).
    const VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    if (vkCreateSampler(device_.get_device(), &sampler_info, nullptr, &sampler_) != VK_SUCCESS)
        throw std::runtime_error("PostProcessPass: failed to create sampler");

    // Histogram bins: the first REAL FrameScratch customer outside geometry — a per-frame
    // transient (rewritten from scratch every frame) placed in the per-slot arena.
    hist_off_ = scratch_->reserve(256 * sizeof(uint32_t));

    // Per-slot host-visible exposure readback ring (16 B each; read frames_in_flight later).
    readback_.resize(frames_in_flight_);
    readback_mapped_.resize(frames_in_flight_);
    for (uint16_t f = 0; f < frames_in_flight_; ++f)
    {
        readback_[f] = allocator_.create_resource(string::gpu::buffer_info{
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
        return context.shader_registry.create(
            context.resources_path / "shaders" / "post.slang",
            [layout, entry](string::gpu::device& dev, const string::gpu::compiled_program& compiled) {
                string::gpu::pipeline p{};
                p.push_constants = compiled.layout.push_constant;
                p.pipeline_layout = string::gpu::pipeline_layout_builder()
                    .set_descriptor_set_layout({ layout })
                    .set_push_constant_ranges({ compiled.layout.push_constant })
                    .build(dev);
                string::gpu::pipeline_builder builder(dev, string::gpu::pipeline_type::COMPUTE);
                for (const auto& stage : compiled.stages)
                    if (stage.stage == VK_SHADER_STAGE_COMPUTE_BIT && stage.entry_point == entry)
                        builder.add_compute_shader_spirv(stage.spirv, stage.entry_point);
                p.pipeline = builder.build_compute_pipeline(p.pipeline_layout);
                p.pipeline_type = string::gpu::pipeline_type::COMPUTE;
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

PostProcessPass::~PostProcessPass()
{
    const auto destroy_program = [&](string::gpu::shader_program* prog) {
        if (!prog) return;
        const string::gpu::pipeline& p = prog->current();
        vkDestroyPipeline(device_.get_device(), p.pipeline, nullptr);
        vkDestroyPipelineLayout(device_.get_device(), p.pipeline_layout, nullptr);
    };
    destroy_program(hist_clear_program_);
    destroy_program(hist_program_);
    destroy_program(hist_reduce_program_);
    destroy_program(bloom_down_program_);
    destroy_program(bloom_up_program_);
    destroy_program(bloom_apply_program_);
    destroy_targets();
    if (color_storage_slot_ != UINT32_MAX)
        descriptor_table_.unbind_storage_view(color_storage_slot_);
    for (string::gpu::resource_id id : readback_) allocator_.destroy_resource(id);
    vkDestroySampler(device_.get_device(), sampler_, nullptr);
}

void PostProcessPass::bind_color_source(uint32_t sampled_slot, string::gpu::resource_id physical_id)
{
    color_sampled_slot_ = sampled_slot;
    color_id_ = physical_id;
    // Fresh storage-image slot for the (new) HDR target's default view — the bloom apply writes
    // it in place. Descriptor updates happen here (init / resize), before any recording.
    if (color_storage_slot_ != UINT32_MAX)
        descriptor_table_.unbind_storage_view(color_storage_slot_);
    color_storage_slot_ = descriptor_table_.bind_storage_view(allocator_.get_image(physical_id).view);
}

void PostProcessPass::destroy_targets()
{
    for (uint32_t s : bloom_mip_slots_) descriptor_table_.unbind_storage_view(s);
    bloom_mip_slots_.clear();
    for (VkImageView v : bloom_mip_views_) vkDestroyImageView(device_.get_device(), v, nullptr);
    bloom_mip_views_.clear();
    if (bloom_image_ != 0)
    {
        descriptor_table_.unbind(bloom_image_, string::gpu::descriptor_type::TEXTURE);
        allocator_.destroy_resource(bloom_image_);
        bloom_image_ = 0;
    }
    bloom_layout_init_ = false;
}

void PostProcessPass::ensure_targets()
{
    if (screen_size.width == 0 || screen_size.height == 0) return;
    const glm::uvec2 screen(screen_size.width, screen_size.height);
    if (bloom_image_ != 0 && screen == bloom_screen_) return;
    destroy_targets();
    bloom_screen_ = screen;
    bloom_base_ = glm::max(screen / 2u, glm::uvec2(1));

    uint32_t mips = uint32_t(std::clamp(cv_bloom_mips().get(), 1, 8));
    while (mips > 1 && (std::min(bloom_base_.x, bloom_base_.y) >> (mips - 1)) < 4) --mips;
    bloom_mips_ = mips;

    bloom_image_ = allocator_.create_resource(string::gpu::image_info{
        .extent = { bloom_base_.x, bloom_base_.y, 1 },
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
        .mip_levels = mips,
    });
    const string::gpu::allocated_image& img = allocator_.get_image(bloom_image_);
    descriptor_table_.bind(bloom_image_, string::gpu::descriptor_type::TEXTURE);
    bloom_sampled_slot_ = descriptor_table_.get_binding_slot(bloom_image_, string::gpu::descriptor_type::TEXTURE);
    descriptor_table_.update_texture(bloom_sampled_slot_, img.view, sampler_);
    for (uint32_t m = 0; m < mips; ++m)
    {
        const VkImageViewCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = img.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1 },
        };
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(device_.get_device(), &vi, nullptr, &view) != VK_SUCCESS)
            throw std::runtime_error("PostProcessPass: failed to create bloom mip view");
        bloom_mip_views_.push_back(view);
        bloom_mip_slots_.push_back(descriptor_table_.bind_storage_view(view));
    }
    STRING_LOG_INFO("[post] bloom chain {}x{} x{} mips", bloom_base_.x, bloom_base_.y, mips);
}

void PostProcessPass::update(float delta_time, uint16_t current_frame)
{
    // Descriptor updates (target recreation) must land before this frame's recording binds the
    // bindless set — update() runs pre-record by construction (the brief-07 UPDATE_AFTER_BIND
    // gotcha).
    ensure_targets();

    // Auto-exposure: consume the metering value this slot's buffer carries (written
    // frames_in_flight frames ago; the frame-slot timeline wait in begin_frame makes it safe).
    if (frame_index_ >= frames_in_flight_ && current_frame < readback_.size())
    {
        ExposureOut out{};
        std::memcpy(&out, readback_mapped_[current_frame], sizeof(out));
        if (cv_exposure_verify().get() && !verify_logged_ && out.total != 0)
        {
            verify_logged_ = true;
            STRING_LOG_INFO("[exposure] histogram total {} (expect {}x{} = {}), kept {} "
                            "(cut {:.2f}/{:.2f}), avg log2 lum {:.3f}",
                            out.total, screen_size.width, screen_size.height,
                            uint64_t(screen_size.width) * screen_size.height, out.kept,
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
                ev100_ += (target - ev100_) * (1.0f - std::exp(-delta_time * rate));
                // Snap once within a millistop: the exponential approach never exactly converges
                // in fp, which would leave a permanent last-ulp EV difference between two runs
                // whose warmup (texture-streaming timing) differed — the run-twice AE=0 gate
                // requires the settled state to be history-free.
                if (std::abs(target - ev100_) < 1e-3f) ev100_ = target;
            }
            String::CompositePass::set_auto_ev100(ev100_);
        }
    }
    ++frame_index_;

    // Declared usages for this frame slot (04e authoring contract): the HDR target is sampled +
    // storage-written at COMPUTE (GENERAL); histogram scratch + readback are compute writes.
    usages.clear();
    usages.push_back({ string::gpu::COLOR_TARGET, String::Access::StorageImageWrite,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT });
    if (scratch_->materialized())
        usages.push_back({ scratch_->buffer(current_frame), String::Access::StorageWrite,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT });
    usages.push_back({ readback_[current_frame], String::Access::StorageWrite,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT });
}

void PostProcessPass::record(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    if (bloom_image_ == 0) return;
    VkCommandBuffer cb = recorder.vk();   // escape: vku::transition_image + record_outline_slot take a raw cb
    VkDescriptorSet set = descriptor_table_.get_set();

    const auto dispatch = [&](string::gpu::shader_program* prog, const PostPush& push,
                              uint32_t gx, uint32_t gy) {
        const string::gpu::pipeline& p = prog->current();
        recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        recorder.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline_layout,
                                      0, 1, &set, 0, nullptr);
        recorder.push_constants(p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(PostPush), &push);
        recorder.dispatch(gx, gy, 1);
    };
    const auto barrier = [&] {
        const VkMemoryBarrier2 mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                           | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                           | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                           | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                           | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        };
        const VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &mb };
        recorder.barrier(dep);
    };

    // Bloom chain lives permanently in GENERAL (07 cubemap pattern): one-time transition, then a
    // single execution barrier orders this frame's writes against last frame's reads.
    const string::gpu::allocated_image& bloom = allocator_.get_image(bloom_image_);
    if (!bloom_layout_init_)
    {
        bloom_layout_init_ = true;
        vku::transition_image(cb, {
            .image = bloom.image,
            .old_layout = VK_IMAGE_LAYOUT_UNDEFINED,
            .new_layout = VK_IMAGE_LAYOUT_GENERAL,
            .src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, .src_access = 0,
            .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dst_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
            .level_count = bloom_mips_,
        });
    }
    else
    {
        barrier();   // cross-frame WAR on the bloom chain (same queue, cross-CB)
    }

    const glm::uvec2 screen(screen_size.width, screen_size.height);
    PostPush base{};
    base.hist = scratch_->materialized() ? scratch_->address(current_frame) + hist_off_ : 0;
    base.readback = allocator_.get_buffer(readback_[current_frame]).device_address;
    base.log_min = kLogMin;
    base.log_inv_range = 1.0f / (kLogMax - kLogMin);

    // --- 1. Auto-exposure metering (histogram over the PRE-bloom resolved HDR) ---
    if (base.hist != 0)
    {
        PostPush push = base;
        dispatch(hist_clear_program_, push, 1, 1);
        barrier();
        push.src_slot = color_sampled_slot_;
        push.src_size = screen;
        dispatch(hist_program_, push, (screen.x + 15) / 16, (screen.y + 15) / 16);
        barrier();
        push.p0 = std::clamp(cv_exposure_cut_low().get(), 0.0f, 0.95f);
        push.p1 = std::clamp(cv_exposure_cut_high().get(), 0.0f, 0.5f);
        dispatch(hist_reduce_program_, push, 1, 1);
    }

    // --- 2. Bloom (r.bloom.enabled; off = the identity/parity lever) ---
    if (cv_bloom_enabled().get())
    {
        // Downsample pyramid. Mip 0 reads the scene (Karis average + firefly clamp).
        for (uint32_t m = 0; m < bloom_mips_; ++m)
        {
            PostPush push = base;
            const glm::uvec2 dst(std::max(bloom_base_.x >> m, 1u), std::max(bloom_base_.y >> m, 1u));
            if (m == 0)
            {
                push.src_slot = color_sampled_slot_;
                push.src_size = screen;
                push.flags = 1u;   // Karis + clamp
                push.p0 = std::max(cv_bloom_clamp().get(), 0.0f);
            }
            else
            {
                push.src_slot = bloom_sampled_slot_;
                push.src_size = glm::uvec2(std::max(bloom_base_.x >> (m - 1), 1u),
                                           std::max(bloom_base_.y >> (m - 1), 1u));
                push.flags = (m - 1) << 8;
            }
            push.dst_slot = bloom_mip_slots_[m];
            push.dst_size = dst;
            dispatch(bloom_down_program_, push, (dst.x + 7) / 8, (dst.y + 7) / 8);
            barrier();
        }
        // Tent upsample-accumulate back to mip 0.
        for (int m = int(bloom_mips_) - 2; m >= 0; --m)
        {
            PostPush push = base;
            const glm::uvec2 dst(std::max(bloom_base_.x >> m, 1u), std::max(bloom_base_.y >> m, 1u));
            push.src_slot = bloom_sampled_slot_;
            push.src_size = glm::uvec2(std::max(bloom_base_.x >> (m + 1), 1u),
                                       std::max(bloom_base_.y >> (m + 1), 1u));
            push.flags = uint32_t(m + 1) << 8;
            push.dst_slot = bloom_mip_slots_[uint32_t(m)];
            push.dst_size = dst;
            push.p0 = std::clamp(cv_bloom_radius().get(), 0.0f, 2.0f);
            dispatch(bloom_up_program_, push, (dst.x + 7) / 8, (dst.y + 7) / 8);
            barrier();
        }
        // Apply into the scene target in place (after the histogram's reads — the barriers above
        // ordered them).
        PostPush push = base;
        push.src_slot = bloom_sampled_slot_;
        push.src_size = bloom_base_;
        push.dst_slot = color_storage_slot_;
        push.dst_size = screen;
        push.p0 = std::max(cv_bloom_intensity().get(), 0.0f);
        dispatch(bloom_apply_program_, push, (screen.x + 7) / 8, (screen.y + 7) / 8);
    }

    // --- 3. Outline/rim slot (brief 10 decides the style; depth + reconstructed normals are
    // available at this point in the frame) ---
    record_outline_slot(cb);
}

}  // namespace sandbox
