#include <string/render/forward_renderer.hpp>

#include <cstdlib>
#include <cstring>

#include <string/core/logger.hpp>

#include <string/render/render_cvars.hpp>

namespace string::render
{
using namespace string;

scene_resources declare_resources(string::frame_graph& fg, VkExtent2D viewport,
                                  VkSampleCountFlagBits samples, uint32_t shadow_res,
                                  uint32_t cascades, VkDeviceSize worklist_bytes,
                                  VkDeviceSize draw_lod_bytes, const scene_backing* backing)
{
    using namespace string;
    scene_resources r;

    // The one image whose backing the renderer still latches — it is not known until the frame
    // acquires it. Everything else the graph owns outright.
    r.swapchain = fg.use_persistent(string::persistent_image_info{ .name = "swapchain", .swapchain = true });

    // The multisampled scene targets and the single-sample HDR they resolve into. Declaring both as
    // colour writes on one pass is what tells the graph there is a resolve; there is no marker and no
    // hook.
    r.color = fg.image({ .name = "scene.color", .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                         .fit = string::viewport_fit::scaled, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                         .samples = samples });
    r.depth = fg.image({ .name = "scene.depth", .format = VK_FORMAT_D32_SFLOAT,
                         .fit = string::viewport_fit::scaled,
                         .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         .aspect = VK_IMAGE_ASPECT_DEPTH_BIT, .samples = samples });
    r.hdr = fg.image({ .name = "scene.hdr", .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                       .fit = string::viewport_fit::scaled,
                       .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                              | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                              | VK_IMAGE_USAGE_STORAGE_BIT });

    // HiZ. The depth history is the one genuinely temporal image in the renderer — GTAO reprojection
    // reads the PREVIOUS frame slot's resolved depth. Its contents outlive the frame, so per brief
    // 21 D3 the APP backs it (scene_backing, one image per frame in flight) and declares TWO
    // persistent handles over the same physicals: identity order for this frame's write, rotated
    // one slot back for last frame's read. The rotation is STATIC — declared once, resolved by
    // frame slot, no per-frame wiring and no reach into graph records.
    {
        std::vector<gpu::resource_id> ring, rotated;
        if (backing != nullptr)
        {
            ring = backing->hiz_depth;
            rotated.resize(ring.size());
            for (std::size_t i = 0; i < ring.size(); ++i)
                rotated[i] = ring[(i + ring.size() - 1) % ring.size()];
        }
        r.hiz_depth = fg.use_persistent(string::persistent_image_info{
            .name = "hiz.depth", .physical = ring });
        r.hiz_depth_prev = fg.use_persistent(string::persistent_image_info{
            .name = "hiz.depth_prev", .physical = rotated });
    }
    // The occlusion pyramid FOLLOWS THE WINDOW: half the next power of two, full chain. Both halves
    // of that are declared relationships, not numbers, so a resize re-backs it without re-authoring —
    // and the reduction chain that builds it is authored once at the maximum depth with the levels
    // this extent does not reach conditioned off (geometry_pass::declare_hiz).
    r.hiz_pyramid = fg.image({ .name = "hiz.pyramid", .format = VK_FORMAT_R32_SFLOAT,
                               .fit = string::viewport_fit::half_pow2,
                               .mip_levels = string::transient_image_info::all_mips,
                               .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                               .sampler = { .mag_filter = VK_FILTER_NEAREST,
                                            .min_filter = VK_FILTER_NEAREST,
                                            .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                            .address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                            .anisotropy = false } });

    // GTAO, half res. `neutral` on the AO target is what a consumer gets when GTAO is toggled off:
    // a flat encoded bent normal with visibility 1, i.e. no occlusion — not black.
    const gpu::sampler_info clamp_linear{ .mag_filter = VK_FILTER_LINEAR, .min_filter = VK_FILTER_LINEAR,
                                          .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                          .address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                          .anisotropy = false };
    r.gtao_raw = fg.image({ .name = "gtao.raw", .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                            .fit = string::viewport_fit::scaled, .viewport_scale = 0.5f,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                            .sampler = clamp_linear });
    r.gtao_ao = fg.image({ .name = "gtao.ao", .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                           .fit = string::viewport_fit::scaled, .viewport_scale = 0.5f,
                           .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                           .sampler = clamp_linear,
                           .neutral = {{ 0.5f, 0.5f, 0.5f, 1.0f }} });

    // Shadow cascades: ONE image each, no ring. The three-deep ring was pure frames-in-flight
    // write-after-read avoidance, which the graph derives — 144 MB down to 48 MB. `neutral` 1.0 is
    // what makes toggling shadows off produce a lit unshadowed scene rather than a black one.
    for (uint32_t c = 0; c < cascades; ++c)
        r.shadow[c] = fg.image({
            .name = "shadow.cascade" + std::to_string(c), .format = VK_FORMAT_D32_SFLOAT,
            .extent = { shadow_res, shadow_res, 1 },
            // TRANSFER_SRC so STRING_CAPTURE_SOURCE=shadowN can actually read one. Without it the
            // copy is invalid usage, which is why that lever never produced a real depth dump.
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
            .sampler = { .mag_filter = VK_FILTER_NEAREST, .min_filter = VK_FILTER_NEAREST,
                         .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                         .address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                         .anisotropy = false,
                         .border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE },
            // REVERSE-Z: 0.0 is the far plane, so 0.0 means "no occluder anywhere" and reads as LIT.
            // It was declared 1.0 — the near plane — which is an occluder in front of everything, so
            // degrading to it shadowed the entire scene. The old `cascade_count = 0` ternary hid that
            // by making the shader skip the lookup entirely; deleting the ternary (step 4) is what
            // exposed it, which is exactly what the per-toggle gate is for.
            .neutral = {{ 0.0f, 0.0f, 0.0f, 0.0f }} });

    // Bloom's mip count is fixed at author time, from the initial viewport — authored-once means the
    // number of declared passes cannot change on resize.
    r.bloom_mips = post_pass::bloom_mip_count(viewport);
    r.bloom = fg.image({ .name = "post.bloom", .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                         .fit = string::viewport_fit::scaled, .viewport_scale = 0.5f, .mip_levels = r.bloom_mips,
                         .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                         .sampler = clamp_linear });

    // --- buffers ---------------------------------------------------------------------------------
    r.froxels = fg.buffer({ .name = "froxels",
                            // One [count, light indices...] record per froxel, and a froxel column
                            // per screen tile — the relationship, stated rather than computed.
                            .tile_pixels = kFroxelTileSize,
                            .bytes_per_tile = froxel_component::bytes_per_tile(),
                            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT });
    // Host-write pacing rings (brief 21 D3): the CPU fills frame N+1's slot while the GPU reads
    // frame N's, so their contents outlive the frame and the APP backs them, one slot per frame in
    // flight. The graph resolves them by slot.
    const auto persist_buf = [&](const char* name, const std::vector<gpu::resource_id>* phys) {
        return fg.use_persistent(string::persistent_buffer_info{
            .name = name, .physical = phys != nullptr ? *phys : std::vector<gpu::resource_id>{} });
    };
    r.transparency_list = persist_buf("transparency.list",
                                      backing != nullptr ? &backing->transparency_list : nullptr);
    // Probe GI accumulators (brief 21 D3): captured and relit tiles PERSIST across the amortized
    // rounds — most frames run neither capture nor relight, and readers must see the real
    // accumulated data on exactly those frames. As transients they were degrade-substituted with
    // the 1x1 neutral whenever no producer pass survived, which blanked a bake that was perfectly
    // good. Contents outlive the frame -> the app backs them (scene_backing), sized from the SAME
    // fit the component uses (probe_gi_component::fit_volume), so neither side can drift.
    const auto persist_img = [&](const char* name, gpu::resource_id id) {
        return fg.use_persistent(string::persistent_image_info{
            .name = name,
            .physical = id != 0 ? std::vector<gpu::resource_id>{ id } : std::vector<gpu::resource_id>{} });
    };
    // IBL products, same category and the same reason (see scene_backing): amortized producers whose
    // contents must survive both an idle frame and a resize.
    r.env_capture     = persist_img("ibl.env_capture",     backing != nullptr ? backing->env_capture : 0);
    r.env_prefiltered = persist_img("ibl.env_prefiltered", backing != nullptr ? backing->env_prefiltered : 0);
    r.dfg_lut         = persist_img("ibl.dfg",             backing != nullptr ? backing->dfg_lut : 0);

    r.gi_irradiance = persist_img("gi.irradiance",     backing != nullptr ? backing->gi_irradiance : 0);
    r.gi_cap_gbuf   = persist_img("gi.capture_gbuf",   backing != nullptr ? backing->gi_cap_gbuf : 0);
    r.gi_cap_albedo = persist_img("gi.capture_albedo", backing != nullptr ? backing->gi_cap_albedo : 0);
    r.gi_visibility = persist_img("gi.visibility",     backing != nullptr ? backing->gi_visibility : 0);

    // The per-probe cube G-buffer, destructively reused between probes — pure write-after-read, no
    // history, so one set serves every probe. Genuinely frame-scoped -> stays a graph transient.
    const gpu::sampler_info gi_sampler{ .mag_filter = VK_FILTER_LINEAR, .min_filter = VK_FILTER_LINEAR,
                                        .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                        .address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .anisotropy = false };
    const auto gi_cube = [&](const char* name, VkFormat fmt, VkImageUsageFlags use, VkImageAspectFlags asp) {
        return fg.image({ .name = name, .format = fmt, .extent = { 32, 32, 1 },
                          .array_layers = 6, .cube = true, .usage = use, .aspect = asp,
                          .sampler = gi_sampler });
    };
    r.gi_cube_albedo = gi_cube("gi.cube_albedo", VK_FORMAT_R16G16B16A16_SFLOAT,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
    r.gi_cube_nd     = gi_cube("gi.cube_nd", VK_FORMAT_R16G16B16A16_SFLOAT,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
    r.gi_cube_depth  = gi_cube("gi.cube_depth", VK_FORMAT_D32_SFLOAT,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                               VK_IMAGE_ASPECT_DEPTH_BIT);

    // GI table/state buffers: uploaded at bake start, read for the volume's whole lifetime —
    // contents outlive the frame, app-backed like the atlases.
    const auto persist_one_buf = [&](const char* name, gpu::resource_id id) {
        return fg.use_persistent(string::persistent_buffer_info{
            .name = name,
            .physical = id != 0 ? std::vector<gpu::resource_id>{ id } : std::vector<gpu::resource_id>{} });
    };
    r.gi_active        = persist_one_buf("gi.active",        backing != nullptr ? backing->gi_active : 0);
    r.gi_offset        = persist_one_buf("gi.offset",        backing != nullptr ? backing->gi_offset : 0);
    r.gi_meshlet_table = persist_one_buf("gi.meshlet_table", backing != nullptr ? backing->gi_meshlet_table : 0);
    r.ibl_sh           = persist_one_buf("ibl.sh",           backing != nullptr ? backing->ibl_sh : 0);

    // Histogram bins are written and consumed within one frame — a true transient; the cross-frame
    // write-after-read against last frame's resolve derives from tracked state.
    r.histogram_bins = fg.buffer({ .name = "post.histogram.bins", .bytes = 256 * sizeof(uint32_t),
                                   .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT });

    // SceneData, the animated light ring and the GPU stats block — host-write pacing rings,
    // app-backed (see scene_backing).
    r.scene_data = persist_buf("scene.data",   backing != nullptr ? &backing->scene_data : nullptr);
    r.joint_palette = persist_buf("anim.palette",
                                  backing != nullptr ? &backing->joint_palette : nullptr);
    r.lights     = persist_buf("scene.lights", backing != nullptr ? &backing->lights : nullptr);
    r.stats      = persist_buf("scene.stats",  backing != nullptr ? &backing->stats : nullptr);

    // The GPU work lists (brief 21 D4): ONE TRANSIENT PER LIST — the camera's opaque and two-sided
    // lists, one per possible cascade, and the shared per-draw LOD. They were regions of a per-frame
    // scratch arena the app had to wire in after the fact; as declarations the graph owns their
    // lifetime, their aliasing and the edges between them, and the cascade chains stop serialising
    // behind the camera's just because they shared an allocation.
    const VkBufferUsageFlags wl_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                      | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                      | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                      | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const auto list = [&](const std::string& name, VkDeviceSize bytes) {
        return fg.buffer({ .name = name, .bytes = bytes, .usage = wl_usage });
    };
    r.worklists.opaque   = list("meshlet.worklist.opaque", worklist_bytes);
    r.worklists.twosided = list("meshlet.worklist.twosided", worklist_bytes);
    for (uint32_t c = 0; c < kMaxCascades; ++c)
        r.worklists.cascade[c] = list("meshlet.worklist.cascade" + std::to_string(c), worklist_bytes);
    r.worklists.draw_lod = list("meshlet.draw_lod", draw_lod_bytes);

    return r;
}
void declare_stream_uploads(string::frame_graph& fg, string::engine_context& ctx)
{
    string::TransferBatch* transfer = &ctx.transfer;
    fg.pass("uploads.stream")
      .toggle([transfer] { return transfer->pending(); })
      .transfer([transfer](string::pass_context& pc) { transfer->record(pc.rec); });
}

forward_renderer::forward_renderer(engine_context& ctx, ::string::renderer& rr,
                                   ::string::assets::registry& assets,
                                   ::string::scene::world& world, const settings& s)
: ctx_(ctx)
, rr_(rr)
, assets_(assets)
, world_(world)
, settings_(s)
{
}

forward_renderer::~forward_renderer() = default;

void forward_renderer::set_crowd_hook(std::function<void(bool)> hook)
{
    if (geo_ != nullptr) geo_->set_crowd_hook(std::move(hook));
}

void forward_renderer::declare(::string::frame_graph& fg)
{
    graph_ = &fg;

    // Mint the content heaps' graph handles + upload their backing (rides the construction-upload
    // flush). Before any pass exists: the geometry pass latches these handles at construction.
    assets_.declare(fg);

    // The bridge: the world's GPU mirror. Spawn budgets default to base content x a 6x6 grid (the
    // crowd stress ceiling) when the app does not size them.
    constexpr uint32_t kGrid = 36;
    const uint32_t base_parts = static_cast<uint32_t>(assets_.mesh_parts().size());
    uint32_t base_joints = 0;
    for (const ::string::assets::skin_binding& sb : assets_.skins()) base_joints += sb.joint_count;
    const uint32_t rows = settings_.row_budget != 0 ? settings_.row_budget
                                                    : std::max(1u, base_parts * kGrid);
    const uint32_t joints =
        settings_.palette_budget != 0 ? settings_.palette_budget : base_joints * kGrid;
    bridge_ = std::make_unique<scene_bridge>(ctx_, assets_, world_, rows, joints);

    geo_ = std::make_unique<geometry_pass>(ctx_, settings_.samples, assets_, *bridge_, mesh_stats_,
                                           settings_.lookdev);

    // The persistent backings BEFORE declaring, so every persistent handle is backed at compile
    // and its descriptor slots bind once, up front (brief 21 D3).
    const ProbeVolume gi_volume =
        probe_gi_component::fit_volume(bridge_->bounds_min(), bridge_->bounds_max());
    backing_.create(ctx_, settings_.viewport, bridge_->frame().cull_max_draws_, gi_volume,
                    geo_->gi_capture_entries(), geo_->palette_joints_total());
    res_ = declare_resources(fg, settings_.viewport, settings_.samples,
                             bridge_->settings().shadow_resolution,
                             bridge_->settings().cascade_count, bridge_->frame().wl_layout_.size,
                             VkDeviceSize{ sizeof(uint32_t) } * bridge_->frame().cull_max_draws_,
                             &backing_);
    const scene_resources& r = res_;
    bridge_->set_joint_palette(r.joint_palette);

    // The composite is CONSTRUCTED first: its exposure state is instance state now, and four of
    // the components below hold the instance (furnace pin, auto-EV publish, UI pre-divide, probe
    // debug view). Declaration order below is unchanged — construction order is not authoring
    // order.
    composite_ = std::make_unique<::string::composite_pass>(ctx_, settings_.swapchain_format);
    froxel_ = std::make_unique<froxel_component>(ctx_);
    ibl_ = std::make_unique<ibl_component>(ctx_);
    gtao_ = std::make_unique<gtao_chain>(ctx_, bridge_.get());
    gi_ = std::make_unique<probe_gi_component>(ctx_, bridge_.get(), ibl_.get(), composite_.get(),
                                               settings_.samples);
    sky_ = std::make_unique<sky_component>(ctx_, settings_.samples);
    shadow_ = std::make_unique<shadow_maps>(ctx_, bridge_.get());
    transparency_ = std::make_unique<sorted_transparency>(ctx_, bridge_.get(), settings_.samples);
    // scene.upload's owner: every SceneData input arrives explicitly (constructor wiring — the
    // never-assigned back-pointer class is impossible by construction).
    uniforms_ = std::make_unique<scene_uniforms>(ctx_, *bridge_, froxel_.get(), ibl_.get(),
                                                 gtao_.get(), gi_.get(), shadow_.get(),
                                                 composite_.get());
    geo_->set_uniforms(uniforms_.get());
    debug_lines_ = std::make_unique<debug_line_pass>(ctx_, bridge_.get(), settings_.samples);
    ui_ = std::make_unique<ui_pass>(ctx_, settings_.samples, composite_.get(), ui_hooks_.atlas,
                                    ui_hooks_.author, ui_hooks_.post_layout, ui_hooks_.deferred);
    post_ = std::make_unique<post_pass>(ctx_, composite_.get());

    const std::span<const string::gpu::image> cascades{ r.shadow,
                                                        bridge_->settings().cascade_count };
    const probe_gi_component::resources gi_res{
        r.gi_irradiance, r.gi_cap_gbuf, r.gi_cap_albedo, r.gi_visibility,
        r.gi_cube_albedo, r.gi_cube_nd, r.gi_cube_depth,
        r.gi_active, r.gi_offset, r.gi_meshlet_table };

    // DECLARATION ORDER IS EXECUTION ORDER, and two orderings are LOAD-BEARING: ibl before gi
    // (relight reads this frame's sky SH) and gi before shadow (relight samples the cascades the
    // shadow passes then rewrite). Owning this order here is the point of the preset — it is no
    // longer the app's to get wrong.
    declare_stream_uploads(fg, ctx_);
    froxel_->declare(fg, r.froxels, r.lights);
    ibl_->declare(fg, r.env_capture, r.env_prefiltered, r.dfg_lut, r.ibl_sh);
    gtao_->declare(fg, r.hiz_depth_prev, r.gtao_raw, r.gtao_ao);
    gi_->declare(fg, gi_res, r.ibl_sh, r.scene_data, cascades, r.color, r.depth);
    shadow_->declare(fg, cascades, r.worklists, r.scene_data, r.joint_palette);
    sky_->declare(fg, r.color, r.depth, [] { return cv_pass_sky().get(); });
    uniforms_->declare(fg, r.scene_data, r.lights, r.stats, cascades, r.gtao_ao,
                       r.env_prefiltered, r.dfg_lut, r.ibl_sh, r.froxels, r.joint_palette);
    geo_->declare(fg, r.color, r.hdr, r.depth, r.hiz_depth, r.hiz_pyramid, r.worklists,
                  r.scene_data, r.lights, r.stats, cascades, r.gtao_ao, r.env_prefiltered,
                  r.dfg_lut, r.ibl_sh, r.froxels, r.joint_palette);
    transparency_->declare(fg, r.color, r.depth, r.transparency_list, r.scene_data, r.stats);
    debug_lines_->declare(fg, r.color, r.depth);
    ui_->declare(fg, r.color);
    post_->declare(fg, r.hdr, r.bloom, r.bloom_mips, r.histogram_bins);
    composite_->declare(fg, r.hdr, r.swapchain);

    // The headless capture target: the resolved HDR, or a raw cascade depth map via
    // STRING_CAPTURE_SOURCE=shadow<N>.
    string::gpu::image capture_target = r.hdr;
    if (const char* csrc = std::getenv("STRING_CAPTURE_SOURCE");
        csrc != nullptr && std::strncmp(csrc, "shadow", 6) == 0)
    {
        const int c = std::atoi(csrc + 6);
        if (c >= 0 && c < static_cast<int>(bridge_->settings().cascade_count))
            capture_target = r.shadow[c];
    }
    rr_.declare_capture(fg, capture_target, composite_.get());

    // The OWNER half of a resize: re-back the viewport-sized depth-history ring and re-point both
    // handles (identity + rotated-one-back), before the renderer's graph half re-binds.
    rr_.on_resize([this](VkExtent2D e) {
        backing_.create_hiz_depth(e);
        const std::vector<string::gpu::resource_id>& ring = backing_.hiz_depth;
        std::vector<string::gpu::resource_id> rotated(ring.size());
        for (std::size_t i = 0; i < ring.size(); ++i)
            rotated[i] = ring[(i + ring.size() - 1) % ring.size()];
        graph_->set_images(res_.hiz_depth, ring);
        graph_->set_images(res_.hiz_depth_prev, rotated);
    });
}

void forward_renderer::tick(float dt)
{
    const std::uint32_t slot = rr_.frame_slot();

    // The bridge re-derives rows + cascade fits + palette windows from the world snapshot, then
    // the palettes land in THIS slot's ring physical (per-frame safety is the RING — the GPU
    // reads an older slot behind its fence).
    bridge_->tick();
    if (bridge_->palette_joints_total() > 0)
    {
        auto* palettes = static_cast<glm::mat4*>(
            backing_.ctx->allocator.get_buffer(backing_.joint_palette[slot])
                .allocation_info.pMappedData);
        bridge_->write_palettes({ palettes, bridge_->palette_joints_total() });
    }
    uniforms_->tick();
    geo_->tick(dt, slot);
    gtao_->tick(rr_.extent(), static_cast<uint16_t>(slot));
    ibl_->tick(ibl_lighting(bridge_->frame()), false);
    gi_->tick();
    froxel_->tick(froxel_params(bridge_->frame()));
    sky_->tick(sky_params(bridge_->frame()));
    ui_->tick(dt, rr_.extent(), slot);
    post_->tick(dt);
    composite_->tick();
}

}  // namespace string::render
