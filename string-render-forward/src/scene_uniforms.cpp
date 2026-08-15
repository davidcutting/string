#include <string/render/scene_uniforms.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <string/core/logger.hpp>
#include <string/vulkan/passes/composite_pass.hpp>
#include <glm/gtc/constants.hpp>

#include <string/render/gtao.hpp>
#include <string/render/probe_gi_component.hpp>
#include <string/render/render_cvars.hpp>
#include <string/render/geometry/froxel_component.hpp>
#include <string/render/geometry/ibl_component.hpp>

namespace string::render
{
using namespace string;

scene_uniforms::scene_uniforms(engine_context& ctx, const scene_bridge& bridge,
                               const froxel_component* froxel, const ibl_component* ibl,
                               const gtao_chain* gtao, probe_gi_component* gi,
                               const shadow_maps* shadow, string::composite_pass* composite)
: bridge_(bridge)
, froxel_(froxel)
, ibl_(ibl)
, gtao_(gtao)
, gi_(gi)
, shadow_(shadow)
, composite_(composite)
, frames_in_flight_(ctx.frames_in_flight)
{
}

void scene_uniforms::declare(::string::frame_graph& fg, ::string::gpu::buffer scene_data,
                             ::string::gpu::buffer lights, ::string::gpu::buffer stats,
                             std::span<const ::string::gpu::image> cascades,
                             ::string::gpu::image gtao_ao, ::string::gpu::image env_prefiltered,
                             ::string::gpu::image dfg_lut, ::string::gpu::buffer ibl_sh,
                             ::string::gpu::buffer froxels, ::string::gpu::buffer joint_palette)
{
    scene_data_ = scene_data;
    lights_buffer_ = lights;
    stats_ = stats;
    cascade_count_ = std::min<uint32_t>(static_cast<uint32_t>(cascades.size()), kMaxCascades);
    for (uint32_t c = 0; c < cascade_count_; ++c) cascades_[c] = cascades[c];
    gtao_ao_ = gtao_ao;
    env_prefiltered_ = env_prefiltered;
    dfg_lut_ = dfg_lut;
    ibl_sh_ = ibl_sh;
    froxels_ = froxels;
    joint_palette_ = joint_palette;

    // SceneData + the light ring, written before anything reads them. Declaring this is what makes
    // the froxel/shadow-slot addresses inside SceneData safe: they are resolved while this records,
    // against this frame's backing, and every consumer's read is a derived edge against this write.
    string::pass_spec upload = fg.pass("scene.upload");
    upload.writes(scene_data_).writes(lights_buffer_)
          .reads(gtao_ao_).reads(env_prefiltered_).reads(dfg_lut_)
          .reads(ibl_sh_).reads(froxels_);
    // Brief 23: the palette ring. The CPU-side write safety is the RING (frames_in_flight
    // physicals + the per-slot fence); declaring the write is what orders the GPU-side readers
    // and keeps this slot's address resolvable at record time.
    if (joint_palette_.valid()) upload.writes(joint_palette_);
    // The registry's skin heap: this pass resolves its address into SceneData (the mesh-stage
    // consumption is declared on the geometry/shadow/transparency passes).
    if (bridge_.frame().skin_buffer_.valid())
        upload.reads(bridge_.frame().skin_buffer_, string::access::storage_read);
    for (uint32_t c = 0; c < cascade_count_; ++c) upload.reads(cascades_[c]);
    // COMPUTE, not transfer: this pass records no GPU commands at all (it fills a host-visible
    // ring and resolves slots) — but the kind picks the default stage for its reads, and a
    // sampled read at the COPY stage is illegal.
    upload.compute([this](string::pass_context& ctx) { record(ctx); });
}

void scene_uniforms::tick()
{
    // The furnace PINS exposure (over auto AND manual) so a radiance-1 environment reads as flat
    // white. Target is exposed = 4.0: filmic transforms map scene 1.0 to only ~80-85% display, so
    // two stops up lands the flat field at display white while staying on the shoulder.
    composite_->set_exposure_override(std::log2(1000.0f / (1.2f * 4.0f)),
                                      bridge_.frame().env.furnace);
}

void scene_uniforms::record(string::pass_context& ctx)
{
    const scene_frame& f = bridge_.frame();
    const uint32_t current_frame = ctx.frame_slot;

    // Read back the stats the GPU wrote when this slot last ran (frames_in_flight frames ago —
    // its fence passed in begin_frame, so no stall).
    if (stats_.valid() && f.draw_count_ > 0)
    {
        if (const void* stats_mapped = ctx.mapped(stats_))
        {
            std::memcpy(&stats_latest_, stats_mapped, sizeof(GpuMeshStats));
            // STRING_STATS_LOG=1: periodic cull-funnel dump, for diagnosing meshlet loss without
            // a GPU capture.
            static const bool log_stats = std::getenv("STRING_STATS_LOG") != nullptr;
            static uint32_t stats_log_frame = 0;
            if (log_stats && (++stats_log_frame % 60u) == 0u)
            {
                const GpuMeshStats& s = stats_latest_;
                STRING_LOG_INFO("[stats] f{} total {} frustum {} cone {} hiz(drawn) {} phase2 {} "
                                "shadow {} tris {}",
                                stats_log_frame, s.meshlets_total, s.after_frustum, s.after_cone,
                                s.after_hiz, s.phase2_drawn, s.shadow_draws, s.triangles);
            }
        }
    }
    // --- Forward+ per-frame GPU data (this frame's ring slot) ----------------------------------
    if (f.draw_count_ == 0 || !scene_data_.valid() || current_frame >= frames_in_flight_) return;

    // Upload this frame's lights (the world's table; empty when the switch is off).
    const uint32_t light_count =
        f.env.lights_enabled ? static_cast<uint32_t>(f.lights.size()) : 0u;
    if (light_count > 0)
    {
        std::memcpy(ctx.mapped(lights_buffer_), f.lights.data(), sizeof(GpuLight) * light_count);
    }

    // Fill SceneData for this frame. The lit shader reads sun/ambient/CSM/froxel state from here.
    SceneData scene{};
    scene.camera_pos = f.camera_pos;
    scene.exposure = composite_->exposure_scale();   // display-referred debug views
    scene.sun_dir = f.env.sun_dir;
    scene.sun_intensity = f.env.sun_intensity;
    scene.sun_color = f.env.sun_color;
    scene.ambient_sky = f.env.sky_zenith;
    // sky_ground is an ALBEDO; mirror sky_ground_radiance() (sky.slang) so this field keeps its
    // "hemispheric ambient, down" radiance meaning.
    {
        const float lum = glm::dot(f.env.sky_zenith, glm::vec3(0.2126f, 0.7152f, 0.0722f));
        const glm::vec3 horizon = glm::mix(f.env.sky_zenith, glm::vec3(lum), 0.6f) * 2.0f;
        const glm::vec3 e_sun = f.env.sun_color * f.env.sun_intensity
                                * glm::clamp(glm::normalize(f.env.sun_dir).y, 0.0f, 1.0f);
        const glm::vec3 e_sky = glm::pi<float>() * 0.5f * (f.env.sky_zenith + horizon);
        scene.ambient_ground = f.env.sky_ground / glm::pi<float>() * (e_sun + e_sky);
    }
    for (uint32_t c = 0; c < f.settings_.cascade_count; ++c)
    {
        scene.cascade_view_proj[c] = f.cascade_view_proj_[c];
        scene.cascade_split[c] = glm::vec4(f.cascade_split_[c], 0, 0, 0);
        scene.cascade_texel[c] = glm::vec4(f.cascade_world_texel_[c], 0, 0, 0);
        scene.cascade_slot[c] =
            glm::uvec4(shadow_ != nullptr ? ctx.slot(cascades_[c]) : 0u, 0, 0, 0);
    }
    // No shadow-off special case: the cascades are TRANSIENTS declaring `neutral = 1.0`, so when
    // the cascade passes are toggled off ctx.slot() already resolves to the neutral fallback.
    scene.cascade_count = f.settings_.cascade_count;
    scene.shadow_texel = 1.0f / static_cast<float>(f.settings_.shadow_resolution);
    scene.shadow_bias = cv_shadow_bias().get();
    scene.shadow_normal_offset_scale = cv_shadow_normal_offset().get();
    scene.cascade_blend = f.settings_.cascade_blend;
    scene.view = f.view;
    // The froxel grid dims + buffer address, from the constructor-injected component. (The old
    // back-pointer was never assigned, so these dims were silently the null-component placeholder
    // (0,0,24,16) — wrong the moment a scene had local lights.)
    const bool have_froxel = froxel_ != nullptr;
    const uint32_t froxel_tx = have_froxel ? froxel_->tiles_x(ctx.extent) : 0;
    const uint32_t froxel_ty = have_froxel ? froxel_->tiles_y(ctx.extent) : 0;
    scene.froxel_dims = glm::uvec4(froxel_tx, froxel_ty, kFroxelDepthSlices, kFroxelTileSize);
    const float near_p = f.near_plane;
    const float far_p = std::min(f.settings_.shadow_depth_range, f.far_plane);
    scene.froxel_planes = glm::vec4(near_p, far_p, 1.0f / std::log(far_p / near_p),
                                    static_cast<float>(light_count));
    scene.lights = light_count > 0 ? ctx.address(lights_buffer_) : 0;
    // The froxel list's address. This pass DECLARES it, so resolving here is exactly what the
    // declaration licenses. MUST be set: lighting.slang dereferences it the moment a scene has
    // local lights, and a null device address is a GPU fault, not a wrong colour.
    scene.froxels = ctx.address(froxels_);
    scene.max_lights_per_froxel = kMaxLightsPerFroxel;
    scene.debug_flags = (f.froxel_heatmap_ ? 1u : 0u) | (f.env.furnace ? 2u : 0u)
                      | (cv_gtao_spec_occ().get() ? 0u : 4u)
                      | ((static_cast<uint32_t>(std::max(cv_light_debug().get(), 0)) & 0xFu) << 4);
    // Brief 07: the sky-IBL products.
    scene.sh = ctx.address(ibl_sh_);
    scene.env_slot = ctx.slot(env_prefiltered_);
    scene.env_mips = ibl_component::env_mips();
    scene.dfg_slot = ctx.slot(dfg_lut_);

    // GTAO decided the go/no-go in its own tick (it runs before this records); read it here.
    const std::vector<DepthHistorySlot>& history = bridge_.depth_history();
    const bool gtao_runs = gtao_ != nullptr && gtao_->runs();
    const uint16_t gtao_prev = gtao_ != nullptr ? gtao_->prev_slot(current_frame) : 0;
    scene.prev_view_proj = gtao_runs && gtao_prev < history.size()
        ? history[gtao_prev].view_proj : glm::mat4(1.0f);
    // The ~0u sentinel is gtao's ONLY sound degrade — gtao.ao holds an ENCODED bent normal, and
    // any constant decodes to a degenerate vector.
    scene.gtao_slot = gtao_ != nullptr && gtao_->has_output() ? ctx.slot(gtao_ao_) : 0xFFFFFFFFu;
    scene.gtao_strength = std::clamp(cv_gtao_strength().get(), 0.0f, 1.0f);
    const glm::uvec2 gtao_extent = gtao_ != nullptr ? gtao_->size() : glm::uvec2(0);
    scene.gtao_w = gtao_extent.x;
    scene.gtao_h = gtao_extent.y;

    // Brief 09b probe GI: the component owns the gate + volume + atlas slots.
    if (gi_ != nullptr) gi_->fill_scene_data(scene, ctx); else scene.probe_gi = 0u;

    // Brief 23 skinning: the compact skin heap + THIS frame slot's palette ring physical.
    scene.skin_stream = f.skin_buffer_.valid() ? ctx.address(f.skin_buffer_) : 0;
    scene.joint_palette = joint_palette_.valid() ? ctx.address(joint_palette_) : 0;

    std::memcpy(ctx.mapped(scene_data_), &scene, sizeof(SceneData));
}

}  // namespace string::render
