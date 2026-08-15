#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <string/platform/user_dirs.hpp>
#include <string/core/job_system.hpp>
#include <string/core/logger.hpp>
#include <string/gpu/command_recorder.hpp>
#include <string/render/geometry_pass.hpp>
#include <string/render/scene_uniforms.hpp>
#include <string/render/render_cvars.hpp>
#include <glm/gtc/constants.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/vulkan/passes/composite_pass.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include "string/gpu/descriptor_allocator.hpp"
#include "string/gpu/resource.hpp"
#include "vulkan/vulkan_core.h"


namespace string::render
{
using namespace string;

namespace
{

// The renderer's debug keys, interned once. Queries use these constants so a typo is a compile
// error; binding goes through the string overload deliberately (brief 17: that is what registers the
// name the rebinding UI reads back).
namespace debug_actions
{
inline constexpr ::string::ActionId freeze_culling   = ::string::action_id("freeze_culling");
inline constexpr ::string::ActionId toggle_culling   = ::string::action_id("toggle_culling");
inline constexpr ::string::ActionId toggle_hiz       = ::string::action_id("toggle_hiz");
inline constexpr ::string::ActionId cycle_debug_view = ::string::action_id("cycle_debug_view");
inline constexpr ::string::ActionId toggle_crowd     = ::string::action_id("toggle_crowd");
inline constexpr ::string::ActionId toggle_lod       = ::string::action_id("toggle_lod");
inline constexpr ::string::ActionId sun_animate      = ::string::action_id("sun_animate");
inline constexpr ::string::ActionId time_back        = ::string::action_id("time_back");
inline constexpr ::string::ActionId time_fwd         = ::string::action_id("time_fwd");
inline constexpr ::string::ActionId toggle_lights    = ::string::action_id("toggle_lights");
inline constexpr ::string::ActionId toggle_heatmap   = ::string::action_id("toggle_heatmap");
}  // namespace debug_actions

// The projected screen-pixel span of a world AABB — the coverage half of the texture-LOD feedback
// (the asset registry turns coverage into a desired mip). Returns 0 when every corner is behind /
// on the near plane or the span is sub-pixel, which the registry maps to its coarse floor.
float aabb_coverage_px(const glm::mat4& view_proj, const glm::vec3& mn, const glm::vec3& mx,
                       VkExtent2D screen)
{
    glm::vec2 lo(std::numeric_limits<float>::max());
    glm::vec2 hi(std::numeric_limits<float>::lowest());
    int in_front = 0;
    for (int i = 0; i < 8; ++i)
    {
        const glm::vec4 corner((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z, 1.0f);
        const glm::vec4 clip = view_proj * corner;
        if (clip.w <= 1e-4f)
        {
            continue;  // behind / on the near plane — skip (a projected point would be meaningless)
        }
        ++in_front;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        const glm::vec2 px = (ndc * 0.5f + 0.5f) * glm::vec2(screen.width, screen.height);
        lo = glm::min(lo, px);
        hi = glm::max(hi, px);
    }
    if (in_front == 0)
    {
        return 0.0f;
    }
    const float span = std::max(hi.x - lo.x, hi.y - lo.y);
    return span <= 1.0f ? 0.0f : span;
}

// Is the world-space AABB inside the frustum? Gribb-Hartmann planes from the view-projection
// (ZERO_TO_ONE clip) on the CPU, for geometry-residency feedback — we want a draw's geometry as
// soon as it's potentially visible.
bool aabb_in_frustum(const glm::mat4& vp, const glm::vec3& mn, const glm::vec3& mx)
{
    const glm::vec4 r0(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    const glm::vec4 r1(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    const glm::vec4 r2(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    const glm::vec4 r3(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);
    const glm::vec4 planes[6] = { r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2 };
    for (int i = 0; i < 6; ++i)
    {
        const glm::vec3 n(planes[i]);
        const glm::vec3 p(n.x >= 0.0f ? mx.x : mn.x, n.y >= 0.0f ? mx.y : mn.y, n.z >= 0.0f ? mx.z : mn.z);
        if (glm::dot(n, p) + planes[i].w < 0.0f)
        {
            return false;
        }
    }
    return true;
}

// One inspector row: static per-draw geometry + the v5 authored name from the cooked blob (via
// the bridge row's registry part); "draw N" only when unnamed.
InspectorDraw make_inspector_draw(uint32_t d, const GpuDrawInfo& info,
                                  std::span<const scene_bridge::row_meta> rows,
                                  std::span<const ::string::assets::mesh_part> parts)
{
    const ::string::assets::mesh_part* part =
        d < rows.size() && rows[d].mesh.index < parts.size() ? &parts[rows[d].mesh.index]
                                                             : nullptr;
    InspectorDraw id;
    id.name = part != nullptr && !part->name.empty() ? part->name : "draw " + std::to_string(d);
    id.index = d;
    id.material =
        part != nullptr && part->material.valid() ? static_cast<int32_t>(part->material.index) : -1;
    id.meshlet_count = info.total_meshlets;
    id.lod_count = info.lod_count;
    id.aabb_min = info.center - glm::vec3(info.radius);
    id.aabb_max = info.center + glm::vec3(info.radius);
    return id;
}

}  // namespace

geometry_pass::geometry_pass(engine_context& context, VkSampleCountFlagBits samples,
                             ::string::assets::registry& assets, scene_bridge& bridge,
                             std::shared_ptr<MeshOverlayStats> overlay_stats, bool lookdev)
: device_(context.device)
, allocator_(context.allocator)
, descriptor_table_(context.descriptor_table)
, input_map_(context.input_map)
, assets_(assets)
, bridge_(bridge)
, gpu_profiler_ctx_(context.gpu_profiler_ctx)
{
    scene_samples_ = samples;
    // These live on the GeometryScene base, and a base's members cannot sit in the derived init
    // list, so seed them first thing in the body (nothing above uses them).
    frames_in_flight_ = context.frames_in_flight;
    overlay_stats_ = std::move(overlay_stats);
    // The whole geometry pass is CVar-toggleable (r.pass.geometry); off -> composite reads the
    // cleared color target. Touch it NOW so it self-registers at construction, before the env
    // override is applied, rather than lazily at frame 1. The toggle is declared in declare().
    (void)cv_pass_geometry();
    // Brief 21 D4: the work lists are graph transients the app declares; nothing is reserved here.
    // Asset-layer split: the cooked scenes were loaded/merged into the APP-OWNED asset registry
    // before this pass was constructed (cook-on-load lives behind its injected provider). Adopt
    // runtime copies of the registry's global tables here — transitional: the GPU heaps move
    // behind the registry next, and these copies die with GeometryScene.
    lookdev_ = lookdev;
    meshlet_model_.meshlets.assign(assets.meshlets().begin(), assets.meshlets().end());
    meshlet_model_.meshlet_vertices.assign(assets.meshlet_vertices().begin(),
                                           assets.meshlet_vertices().end());
    meshlet_model_.meshlet_triangles.assign(assets.meshlet_triangles().begin(),
                                            assets.meshlet_triangles().end());
    meshlet_model_.total_meshlets = assets.total_meshlets();

    // Latch the registry's heap graph handles into the shared scene tables: every consumer
    // (this pass, shadow, transparency, probe GI) declares its own read and resolves the address
    // through its pass_context.
    vertex_buffer_ = assets.view().vertices;
    meshlet_buffer_ = assets.view().meshlets;
    meshlet_vertices_ = assets.view().meshlet_vertices;
    meshlet_triangles_ = assets.view().meshlet_triangles;
    skin_buffer_ = assets.view().skin_stream;

    // The draw/row state is the BRIDGE's now (world instances x registry parts): wire the shared
    // scene fields to its table. The pass owns no scene content — only the technique.
    draw_info_buffer_ = bridge.draw_info_buffer();
    draw_info_mapped_ = bridge.mapped();
    draw_count_ = bridge.row_count();
    base_draw_count_ = bridge.row_count();
    active_draw_count_ = bridge.row_count();
    scene_aabb_min_ = bridge.bounds_min();
    scene_aabb_max_ = bridge.bounds_max();

    STRING_LOG_INFO("[load] cooked scenes: {} files ({} fresh, {} cooked in-process) in {:.1f} ms",
                    assets.stats().files, assets.stats().cooked_hits, assets.stats().cooked_misses,
                    assets.stats().load_ms);
    STRING_LOG_INFO("scene loaded: {} files, {} vertices, {} draws, {} materials, {} textures, {} meshlets",
                    assets.stats().files, assets.vertices().size(), bridge.row_count(),
                    assets.materials().size(), assets.textures().size(),
                    meshlet_model_.total_meshlets);

    // Camera framing bounds: the bridge's world-space union of row AABBs; unit box for an empty
    // world so the framing maths cannot produce NaNs.
    glm::vec3 aabb_min = bridge.bounds_min();
    glm::vec3 aabb_max = bridge.bounds_max();
    if (draw_count_ == 0
        || !(aabb_min.x <= aabb_max.x && aabb_min.y <= aabb_max.y && aabb_min.z <= aabb_max.z))
    {
        aabb_min = glm::vec3(-1.0f);
        aabb_max = glm::vec3(1.0f);
    }

    // --- GPU-driven draw data ------------------------------------------------------------------
    if (draw_count_ > 0)
    {
        STRING_LOG_INFO("[meshlet] {} draws -> {} meshlets, {} vtx-remap, {} tri-words (cooked)",
                        draw_count_, meshlet_model_.total_meshlets,
                        meshlet_model_.meshlet_vertices.size(),
                        meshlet_model_.meshlet_triangles.size());
        // Worklist layout + visbits + pipelines (the DrawInfo table itself is the bridge's).
        build_meshlet_gpu(context);
    }

    // The bounds the camera is about to be framed on. Printed because "I loaded my asset and see
    // nothing" and "my asset is somewhere unexpected" are the same symptom, and this is the number
    // that separates them — it is also what you need to aim STRING_CAM at a specific part of a model.
    STRING_LOG_INFO("[scene] world bounds ({:.2f},{:.2f},{:.2f}) .. ({:.2f},{:.2f},{:.2f})",
                    aabb_min.x, aabb_min.y, aabb_min.z, aabb_max.x, aabb_max.y, aabb_max.z);

    // The CAMERA is the world's now (scene-layer split): framing, the fly-control bindings, the
    // STRING_CAM pose and the TOD/lights levers are the APP's to apply against world.camera()/
    // env(). What this pass binds is its own renderer-debug keys.
    (void)aabb_min;
    (void)aabb_max;
    input_map_.bind_button("freeze_culling", string::KeyCode::F);
    input_map_.bind_button("toggle_culling", string::KeyCode::C);
    input_map_.bind_button("toggle_hiz", string::KeyCode::O);
    input_map_.bind_button("cycle_debug_view", string::KeyCode::V);
    input_map_.bind_button("toggle_crowd", string::KeyCode::K);
    input_map_.bind_button("toggle_lod", string::KeyCode::G);

    // Brief 06: the improvised STRING_* levers are CVar-backed. Seed the runtime-mutable toggles;
    // the F/C/O/V/G/K keys still flip the members live in tick().
    hiz_enabled_ = cv_hiz_enabled().get();
    lod_enabled_ = cv_lod_enabled().get();
    cull_enabled_ = cv_cull_enabled().get();
    debug_view_ = cv_debug_view().get();
    // r.crowd.enabled (STRING_CROWD) arms the crowd hook at first tick (headless benchmark).
    crowd_enabled_ = cv_crowd_enabled().get();

    // Brief 20: the hand-written `usages` vector is GONE. What this pass touches is stated once, in
    // declare(), and that single statement drives both ordering and barrier derivation.

    // Procedural sky background is now the standalone SkyPass (brief 11 step 3).

    // --- Cascaded shadow maps + Forward+ scene/light/froxel buffers ---------------------------
    if (draw_count_ > 0)
    {
        // scene_aabb_min_/max_ came from the bridge above (world-space union of row AABBs).
        // Shadow sampler + cascade images are allocated by the ShadowMaps component ShadowPass owns.

        // --- Per-frame SceneData SSBO ring (device-addressed, persistent-mapped) ---
        // Brief 16 M1: a registry-owned PerFrame buffer (was a hand-managed [frame] ring of
        // resource_ids + mapped pointers). The registry allocates one physical per frame-in-flight
        // and owns them; resolve address/mapped by slot via resources->{address,mapped}(scene_buffer_, f).

        // Debug controls: T animate sun (time-of-day), [ / ] scrub it, L toggle local lights,
        // H toggle the froxel heatmap.
        // T/[/]/L (time-of-day + lights) are SCENE controls — the app binds them against
        // world.env() now. The froxel heatmap stays: it is this renderer's debug view.
        input_map_.bind_button("toggle_heatmap", string::KeyCode::H);
    }
}

// --- Brief 07: dynamic sky IBL --------------------------------------------------------------------
// Two small RGBA16F cubemaps (capture chain + prefiltered roughness ladder), the 9-coefficient SH
// buffer and the split-sum DFG LUT, plus the five compute pipelines that fill them (ibl.slang).
// The cubemaps live in GENERAL layout for their whole life (compute writes + sampled reads both
// legal there; 128px — layout-optimal compression is irrelevant), which keeps the intra-pass sync
// to plain memory barriers. The DFG LUT is baked once and parked in SHADER_READ_ONLY.

// Record the sky-IBL update chain: capture -> capture mip chain -> SH projection + GGX prefilter
// ladder. Runs only on frames where the sun moved past the trigger (see update()) — the whole
// chain is a single-frame update, so the ambient is always self-consistent (no popping). The DFG
// LUT bake rides the first call. All barriers here are the documented INTRA-pass class (like the
// HiZ mip chain): everything is produced and consumed by this pass; the SH buffer's fragment-read
// edge is graph-declared (usages) and the final memory barrier makes the image writes visible to
// the fragment stage.



// Upload the meshlet heaps, build the DrawInfo table (materials + transforms + bounds + LOD ranges),
// allocate the visibility bitfield + stats ring, and create the task/mesh/HiZ/reset pipelines.

// Build (or tear down) the crowd stress scene: duplicate every base draw across a kCrowdGrid x
// kCrowdGrid grid of translated copies, to prove 500+-crowd geometry throughput (brief M6). Crowd
// draws are extra DrawInfo entries referencing the SAME meshlet buffers — only the transform (and
// thus the world-space bounds) differ. Visibility bits are shared with the base draws (conservative
// for the shared meshlet ids — fine for a throughput stress test).

// (Re)create the HiZ pyramid for the current screen size if it changed. Power-of-two conservative
// sizing (each mip is half, rounded down, min 1); R32F mip chain. Binds the whole-chain sampled slot
// (binding 1) + a storage-image slot per mip (binding 2) for the downsample compute.

// --- Brief 09: GTAO targets ------------------------------------------------------------------------
// Half-res RGBA8 (rgb = world bent normal, a = visibility): one shared raw target (produced and
// denoised within one record hook) + one final target per frame slot (this frame's fragments
// sample it while the next frame's chain rewrites its own slot).

// Record the GTAO chain: horizon-search AO + bent normal from the
// PREVIOUS slot's resolved depth, then the spatial denoise into this slot's final target.
// Barriers are the documented cross-frame local class (07 precedent: same-queue, cross-CB;
// hz.depth is untracked pass-managed state, its resolve re-discards from UNDEFINED).

// Brief 03b: dispatch the GPU draw-cull compute to build a work list (commands[] + records[] + count)
// for one consumer. `prepass` forces LOD0 (and skips the LOD histogram) for the HiZ depth prepass;
// the camera list (prepass=false) selects LODs and writes the histogram, and is reused by the main
// pass + all 3 shadow cascades. The list buffer's count is zeroed here, then the compute appends.
// Leaves a barrier making commands[]/records[]/count visible to DRAW_INDIRECT + task-shader reads.

// Brief 04c COMPACTION PHASE for one worklist: scan_blocks -> scan_carry -> fill, then a barrier so the
// resulting commands[] + records[] + count are visible to the task shader / indirect draw. The three
// passes are separated by storage barriers (each reads the previous pass's writes). The fill copies the
// per-draw selected LOD from `draw_lod` into each surviving draw's record.

// Brief 04: build the sorted transparency work list on the CPU. BLEND draws (excluded from the opaque
// GPU lists) are sorted BACK-TO-FRONT by their world-space bounding-sphere distance to the eye, then
// written as a compacted commands[] + records[] + count into the host-visible per-frame buffer. v1
// uses LOD0 for every transparent draw (counts are small; per-draw sort only — no per-meshlet/OIT).
// Returns the number of draws written (indirect count). Runs on the render thread (host-visible write).

// Brief 04: draw the sorted transparency list (after opaque + sky). Same push as the main pass but
// blend_pass=1 (fragment outputs base-color alpha) via the transparency pipeline (blend on, depth
// test vs opaque depth, no depth write). HiZ stays valid (occlusion vs opaque depth is fine).

// Brief 04c (resolution b): the main pass draws the compacted camera list with ONE
// vkCmdDrawMeshTasksIndirectCountEXT. The compaction phase wrote the DENSE commands[] + records[] +
// count into `wl` (one command per surviving draw, ascending draw order); the task shader reads its
// {draw_index, lod} via SV_DrawIndex. groupCountX per command = ceil(LOD meshlets / 32). Command-index
// ordering across the draws is what pins coplanar depth-tie winners run-to-run (AE=0).

// Brief 04e M4: froxel light binning — one thread per froxel bins the local lights into
// per-froxel index lists the lit fragment shader reads. DEPENDENCY-FREE within the frame (reads
// only the host-written light SSBO ring), so the renderer places it on an async compute lane
// when the hardware exposes one (timeline edge + queue-family ownership transfer derived from the
// declarations), or records it inline on the main queue otherwise. No Tracy zone here: the
// renderer wraps the whole async chain in the LANE's own GPU context (a pass-side zone would
// use the main-queue context and produce bogus timestamps on the async queue).






geometry_pass::~geometry_pass()
{
    auto destroy_program = [this](::string::gpu::shader_program* prog) {
        if (!prog) return;
        const ::string::gpu::pipeline& p = prog->current();
        vkDestroyPipeline(device_.get_device(), p.pipeline, nullptr);
        vkDestroyPipelineLayout(device_.get_device(), p.pipeline_layout, nullptr);
    };
    // sky + froxel + IBL programs torn down in their own passes' destructors now (brief 11 step 3).
    destroy_program(meshlet_program_);
    destroy_program(meshlet_twosided_program_);
    destroy_program(hiz_program_);
    destroy_program(reset_program_);
    destroy_program(draw_cull_program_);
    destroy_program(expand_scan_blocks_program_);
    destroy_program(expand_scan_carry_program_);
    destroy_program(expand_fill_program_);
    // Brief 09b probe GI programs.

    // Brief 20: the HiZ pyramid and its per-mip slots are graph resources — nothing to unbind.
    // Brief 21 D4: the work lists are graph transients — the graph frees them.
    // The content heaps + every texture are the ASSET REGISTRY's; the DrawInfo table is the SCENE
    // BRIDGE's (it destroys it — destroying the wired copy here was a double-destroy).
}

void geometry_pass::tick(float delta_time, uint32_t current_frame)
{
    // Mirror this frame's presentation extent from the bridge snapshot FIRST — the HiZ shape below
    // derives from it.
    screen_size = bridge_.frame().screen_size;
    // The HiZ dispatch shape and the two-phase decision are CPU state, and they belong HERE rather
    // than inside a recording callback: the graph's toggles are evaluated at execute, which is after
    // tick, so a predicate reading state that a record body sets is reading last frame's answer.
    if (meshlet_program_ != nullptr && draw_info_mapped_ != nullptr)
        ensure_hiz(static_cast<uint16_t>(current_frame));
    // Active only when HiZ is enabled AND geometry is resident (warmup done). When inactive the pass
    // renders single-pass (phase 0), everything unconditionally — exactly like HiZ-off.
    two_phase_active_ = hiz_enabled_ && hiz_program_ != nullptr && stream_frame_ > 1;

    // Toggle the debug frozen culling frustum. On freeze, snapshot the current view-projection;
    // the camera keeps moving but the cull test stays against the snapshot, so culled geometry
    // becomes visible as it leaves the frozen view.
    if (input_map_.pressed(debug_actions::freeze_culling))
    {
        mesh_cull_frozen_ = !mesh_cull_frozen_;
        if (mesh_cull_frozen_)
        {
            mesh_frozen_view_proj_ = bridge_.frame().view_proj;
            mesh_frozen_camera_pos_ = bridge_.frame().camera_pos;  // cone/HiZ/LOD eye freezes too
        }
        STRING_LOG_INFO("Cull frustum {}", mesh_cull_frozen_ ? "FROZEN (debug)" : "live");
    }
    if (input_map_.pressed(debug_actions::toggle_culling))
    {
        cull_enabled_ = !cull_enabled_;
        STRING_LOG_INFO("GPU frustum culling {}", cull_enabled_ ? "ON" : "OFF (debug)");
    }

    // --- Brief 03 meshlet-path debug controls ---
    if (input_map_.pressed(debug_actions::toggle_hiz))
    {
        hiz_enabled_ = !hiz_enabled_;
        STRING_LOG_INFO("HiZ occlusion {}", hiz_enabled_ ? "ON" : "OFF");
    }
    if (input_map_.pressed(debug_actions::cycle_debug_view))
    {
        debug_view_ = (debug_view_ + 1) % 4;
        static const char* names[] = { "none", "meshlet-id", "LOD-level", "occlusion-reject" };
        STRING_LOG_INFO("Meshlet debug view: {}", names[debug_view_]);
    }
    if (input_map_.pressed(debug_actions::toggle_lod))
    {
        lod_enabled_ = !lod_enabled_;
        STRING_LOG_INFO("Discrete LOD select {}", lod_enabled_ ? "ON" : "OFF (LOD0)");
    }
    if (input_map_.pressed(debug_actions::toggle_crowd))
    {
        crowd_enabled_ = !crowd_enabled_;
        // The crowd is CONTENT now, not a renderer table trick: the app's hook spawns/despawns
        // the grid entities in the world and the bridge re-derives the rows.
        if (crowd_hook_) crowd_hook_(crowd_enabled_);
        STRING_LOG_INFO("Crowd stress scene {}", crowd_enabled_ ? "ON" : "OFF");
    }

    // (Time-of-day + lights keys moved to the app — they drive world.env() now.)
    if (input_map_.pressed(debug_actions::toggle_heatmap))
    {
        froxel_heatmap_ = !froxel_heatmap_;
        STRING_LOG_INFO("Froxel heatmap {}", froxel_heatmap_ ? "ON" : "OFF");
    }
    // (Time-of-day, sun palette + the cascade fit are the world's/bridge's now; the furnace
    // exposure pin lives in scene_uniforms; light animation died with the vestigial stress set.)

    // Publish this pass's cull-debug toggles into the bridge frame — the transparency pass and
    // scene.upload read them at record, and the pass no longer shares a struct with anyone.
    bridge_.set_cull_debug(cull_enabled_, mesh_cull_frozen_, mesh_frozen_view_proj_,
                           mesh_frozen_camera_pos_, debug_view_, froxel_heatmap_);

    // The bridge already re-derived this frame's rows (the app ticks world -> bridge -> passes);
    // refresh the shared counts + the residency-driven visbits invalidation.
    draw_count_ = bridge_.row_count();
    active_draw_count_ = bridge_.row_count();
    if (bridge_.take_residency_changed())
    {
        // Brief 04d: a residency change makes those rows' meshlets "new" — their persistent
        // visibility bits are stale; clear so every meshlet re-validates via phase 2 next frame.
        visbits_clear_pending_ = true;
    }

    // Residency feedback — textures and geometry both driven by the SAME per-row frustum
    // visibility, through the asset registry (which owns the streamers, the budgets and the
    // texture-LOD heuristic): the pass reports "this part is on screen at this coverage", the
    // registry does the rest.
    const glm::mat4 vp = bridge_.frame().view_proj;
    assets_.begin_frame();
    visibility_scratch_.clear();
    const std::span<const scene_bridge::row_meta> rows = bridge_.rows();
    for (const scene_bridge::row_meta& row : rows)
    {
        if (!aabb_in_frustum(vp, row.aabb_min, row.aabb_max)) continue;
        visibility_scratch_.push_back(::string::assets::visibility_sample{
            .mesh = row.mesh,
            .coverage_px = aabb_coverage_px(vp, row.aabb_min, row.aabb_max, screen_size) });
    }
    assets_.observe(visibility_scratch_);
    assets_.tick();

    // Crowd seeded from the headless lever on the first tick (r.crowd.enabled / STRING_CROWD).
    if (crowd_enabled_ && !crowd_seeded_)
    {
        crowd_seeded_ = true;
        if (crowd_hook_) crowd_hook_(true);
    }

    // --- Brief 03b: LOD select moved to the GPU draw-cull compute (record_draw_cull). Here we only
    // read back the stats the GPU wrote earlier + publish the overlay state. ---
    if (draw_info_mapped_)
    {
        // The stats readback lives in scene.upload (scene_uniforms) now; publish its latest here.

        // Publish the overlay state for the UI author (shared_ptr seam; same render thread).
        if (overlay_stats_)
        {
            if (uniforms_ != nullptr) overlay_stats_->stats = uniforms_->stats_latest();
            overlay_stats_->hiz_enabled = hiz_enabled_;
            overlay_stats_->crowd_enabled = crowd_enabled_;
            overlay_stats_->debug_view = debug_view_;
            overlay_stats_->total_meshlets = meshlet_model_.total_meshlets;
            overlay_stats_->draw_count = draw_count_;

            // Brief 06: publish the camera + scene snapshot for the debug-line pass and the
            // scene/draw inspector. view_proj/camera + lights refresh every frame; the draw table is
            // rebuilt only when its size changes (load / crowd toggle) — the per-draw geometry
            // (bounds, meshlet/LOD counts) is static, only the resident flag is live.

            if (draw_info_mapped_ && overlay_stats_->draws.size() != draw_count_)
            {
                overlay_stats_->draws.clear();
                overlay_stats_->draws.reserve(draw_count_);
                for (uint32_t d = 0; d < draw_count_; ++d)
                    overlay_stats_->draws.push_back(
                        make_inspector_draw(d, draw_info_mapped_[d], bridge_.rows(),
                                            assets_.mesh_parts()));
            }
            // Live residency flag per draw (streamer writes it into draw_info_mapped_).
            if (draw_info_mapped_)
                for (uint32_t d = 0; d < overlay_stats_->draws.size(); ++d)
                    overlay_stats_->draws[d].resident = draw_info_mapped_[d].resident != 0;

            overlay_stats_->lights.clear();
            overlay_stats_->lights.reserve(bridge_.frame().lights.size());
            for (const GpuLight& L : bridge_.frame().lights)
            {
                InspectorLight il;
                il.position = glm::vec3(L.position_radius);
                il.range = L.position_radius.w;
                il.color = glm::vec3(L.color_intensity);
                il.spot = L.direction_type.w > 0.5f;
                overlay_stats_->lights.push_back(il);
            }
        }

        // Periodic culling-stats log (the numbers the UI overlay also shows). Sampled a few times so
        // the smoke run captures meaningful frustum/cone/HiZ reductions without spamming.
        if (stream_frame_ == 30 || stream_frame_ == 120 || stream_frame_ == 600)
        {
            static const GpuMeshStats none{};
            const GpuMeshStats& s = uniforms_ != nullptr ? uniforms_->stats_latest() : none;
            // Brief 04d: with two-phase active, meshlets_total/after_frustum/after_cone DOUBLE-count
            // (the task shader runs once per phase per meshlet). after_hiz = phase1+phase2 DRAWN
            // (the union == the visible set); phase2 = the disocclusion complement drawn this frame.
            STRING_LOG_INFO("[mesh-cull] frame {}: meshlets {} -> frustum {} -> cone {} -> hiz {} "
                            "(phase2 {}) (LOD draws {}/{}/{}/{}){}",
                            stream_frame_, s.meshlets_total, s.after_frustum, s.after_cone, s.after_hiz,
                            s.phase2_drawn,
                            s.draws_per_lod[0], s.draws_per_lod[1], s.draws_per_lod[2], s.draws_per_lod[3],
                            crowd_enabled_ ? " [crowd]" : "");
            // Brief 04 M4: per-cascade shadow draw-cull. "before" = every resident draw dispatched for
            // every cascade (resident_draws x cascade_count); "after" = draws surviving the per-cascade
            // light-frustum reject (stats[9], summed across cascades).
            const uint32_t shadow_before = active_draw_count_ * bridge_.settings().cascade_count;
            STRING_LOG_INFO("[shadow-cull] cascades {}: shadow draws {} -> {} (per-cascade light-sphere reject)",
                            bridge_.settings().cascade_count, shadow_before, s.shadow_draws);
            // (The IBL amortization log moved out with the back-pointer bus.)
        }
    }

    ++stream_frame_;
}

// The three resets are three DECLARED PASSES, because they are three producer-consumer stages with
// different consumers: the work lists feed the expand computes and the indirect draws, the stats block
// feeds the cull's histogram atomics, the visibility bits feed the task shaders. As one pass they
// needed two hand-written memory barriers to order themselves against what came next; as declarations
// the graph derives both, and each can be conditioned on its own one-shot predicate.

// Zero every work list ONCE, before anything reads one. They are device-local and therefore
// uninitialised at allocation, and each `commands[]` region feeds draw_mesh_tasks_indirect_count — an
// arbitrary task-group count out of uninitialised memory is a GPU hang, not a wrong picture. Once, not
// once per slot: a transient is single-backed (D3).
void geometry_pass::record_reset_lists(string::pass_context& ctx)
{
    const auto zero = [&](string::gpu::buffer b) {
        if (const ::string::gpu::resource_id id = ctx.id(b); id != 0)
            ctx.rec.fill_buffer(allocator_.get_buffer(id).buffer, 0, VK_WHOLE_SIZE, 0u);
        else
            lists_zeroed_ = false;   // not backed yet — try again next frame
    };
    lists_zeroed_ = true;
    zero(wl_.opaque);
    zero(wl_.twosided);
    zero(wl_.draw_lod);
    for (const string::gpu::buffer& c : wl_.cascade) zero(c);
}

// Zero this frame's stats block. The barrier that used to follow — ordering it before the draw-cull
// compute's histogram atomics — is the cull passes' own declared write of the same buffer.
void geometry_pass::record_reset_stats(string::pass_context& ctx)
{
    const ::string::gpu::pipeline& rp = reset_program_->current();
    const ResetPush rpush{
        .stats = ctx.address(stats_),
        .stats_words = sizeof(GpuMeshStats) / 4,
        ._pad = 0,
    };
    ctx.rec.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, rp.pipeline);
    ctx.rec.push_constants(rp.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(ResetPush), &rpush);
    ctx.rec.dispatch(1, 1, 1);
}

// Clear the persistent visibility bitfield + last-LOD on demand (first frame, teleport, a full
// streaming reset). A cleared bitfield is CORRECT — every meshlet takes phase 2 for one frame. Freeze
// (F) must NOT clear: it would erase the frozen visibility the debug eye is inspecting. The barrier
// that used to follow is derived from the task shaders' declared reads of the same buffers.
void geometry_pass::record_reset_visbits(string::pass_context& ctx)
{
    ctx.rec.fill_buffer(allocator_.get_buffer(visbits_buffer_).buffer, 0, VK_WHOLE_SIZE, 0u);
    ctx.rec.fill_buffer(allocator_.get_buffer(prev_draw_lod_buffer_).buffer, 0, VK_WHOLE_SIZE, 0u);
    visbits_clear_pending_ = false;
}


// Brief 11 M2: probe-GI static capture (once) + dynamic relight (amortized). Scheduled by the
// standalone GiPass in the compute prepass — ordered after IblPass (relight's sky-SH source is this
// frame's) and before ShadowPass (the relight->shadow WAR edge). All barriers are internal.
// Returns true if it recorded work.

void geometry_pass::record_phase1(string::pass_context& ctx)
{
    // The froxel buffer's device address is latched HERE rather than in update(), where it used to
    // depend on FroxelPass having updated first — an ordering nothing enforced. By record() time
    // every pass has updated, so this is always the live address. Patched straight into the
    // persistently-mapped SceneData for this frame slot; the GPU has not read it yet (that happens
    // after submit), and this pass is the only consumer of the field.
    //
    // Done BEFORE the draw_count_ early-out: the transparency pass shares this SceneData.
    // Brief 20: the late froxel-address patch is DELETED. It existed because SceneData was filled in
    // update() but the froxel buffer's address was only valid after another pass's update() had run —
    // an ordering nothing enforced, and the one that device-lost the machine. SceneData is now filled
    // in scene.upload at RECORD time, where ctx.address(froxels) is this frame's real address.

    if (draw_count_ == 0)
    {
        return;
    }

    // The procedural sky was drawn by the standalone SkyPass (first pass of this MSAA group) —
    // brief 11 step 3. geometry_pass::record() now starts straight at the meshlet draw path.

    // --- Brief 03/04d: task/mesh meshlet draw path (the scene geometry front-end) ----------------
    if (meshlet_program_ && draw_info_mapped_)
    {
        if (two_phase_active_)
        {
            // Brief 04d PHASE 1: render meshlets marked visible LAST frame (bit set) directly into the
            // MSAA color+depth. No HiZ test (the pyramid isn't built yet). The renderer then MIN-resolves
            // this depth -> hz.depth, the hiz.build pass builds the pyramid, and geometry.phase2 draws
            // phase 2 (the disocclusion complement) + transparency.
            STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "main-draw")
            record_opaque_phase(ctx, /*phase*/ 1u, pyramid_, scene_data_, stats_);
        }
        else
        {
            // Single-pass (HiZ off / warmup): frustum+cone+HiZ, then transparency, all in one render
            // group.
            {
                STRING_PROFILE_GPU_ZONE(gpu_ctx(), command_buffer, "main-draw")
                record_opaque_phase(ctx, /*phase*/ 0u, pyramid_, scene_data_, stats_);
            }
            // Brief 09b: probe-debug spheres after opaque. Brief 11 M2: transparency moved OUT to the
            // standalone TransparencyPass (drawn last in the reopened MSAA group — same position).
            // Brief 20: the probe-debug spheres are their own declared pass (gi.debug) now, drawn into
            // the same group by its own declaration rather than called from inside this record.

        }
    }
}

// Brief 04d: draw the opaque one-sided + two-sided camera lists at `phase` (0 legacy / 1 bit-set /
// 2 bit-clear+HiZ+update). Assumes an MSAA scene render pass is already open (record_phase1 for
// phase 1/0, record_phase2 for phase 2). Shared by both phases — the task shader does the partitioning.

// Brief 04d: the depth-resolve target the renderer MIN-resolves the phase-1 MSAA depth into (the
// pyramid's single-sample mip-0 source). Valid only when the two-phase path is active this frame.
// Brief 04d: build the HiZ pyramid from the MIN-resolved phase-1 depth (hz.depth), OUTSIDE rendering.
// The renderer already resolved msaa_depth_ -> hz.depth (reverse-Z MIN = farthest) at phase-1's
// EndRendering and left it in DEPTH_ATTACHMENT layout. Reuses the exact mip-0 uniform-stretch +
// 2x2 min chain the old prepass path used; only the depth SOURCE changed (resolve, not a re-draw).
// Author every pass this subsystem owns. The two-phase occlusion path is three real passes sharing
// this object — phase 1 draws what was visible last frame, the HiZ chain reduces the resolved depth,
// phase 2 draws the disocclusion complement — and the graph orders them from what they declare, not
// from a hook that told the renderer where to break the group.
void geometry_pass::declare(string::frame_graph& fg, string::gpu::image color, string::gpu::image resolve,
                            string::gpu::image depth,
                            string::gpu::image hiz_depth, string::gpu::image hiz_pyramid,
                            const WorklistSet& worklists, string::gpu::buffer scene_data,
                            string::gpu::buffer lights, string::gpu::buffer stats,
                            std::span<const string::gpu::image> cascades, string::gpu::image gtao_ao,
                            string::gpu::image env_prefiltered, string::gpu::image dfg_lut,
                            string::gpu::buffer ibl_sh, string::gpu::buffer froxels,
                            string::gpu::buffer joint_palette)
{
    scene_data_ = scene_data;
    joint_palette_ = joint_palette;
    lights_buffer_ = lights;
    stats_ = stats;
    wl_ = worklists;
    pyramid_ = hiz_pyramid;
    gtao_ao_ = gtao_ao;
    env_prefiltered_ = env_prefiltered;
    dfg_lut_ = dfg_lut;
    ibl_sh_ = ibl_sh;
    froxels_ = froxels;
    for (std::size_t c = 0; c < cascades.size() && c < cascades_.size(); ++c) cascades_[c] = cascades[c];

    // The persistent per-meshlet visibility bitfield. This pass allocates and owns the buffer (it is
    // deliberately NOT ring-buffered — the temporal state accumulates across frames), so the graph
    // adopts it rather than allocating it: use_persistent manages usage only.
    //
    // Declaring it is LOAD-BEARING, not bookkeeping. The task shaders read-modify-write these bits
    // every two-phase frame, and storage_write's scope is READ|WRITE, so one declaration on each
    // phase derives BOTH edges that matter: the previous frame's phase-2 writes before this frame's
    // phase-1 reads (the cross-frame RAW — brief 04d's flicker fix), and phase 1 before phase 2
    // within the frame (the WAW/RMW). Without them the bitfield is raced, the "visible last frame"
    // set goes stale or garbage, and phase 2 stops picking up newly disoccluded meshlets — which is
    // invisible while the camera is still and tears the image apart the moment it moves.
    if (visbits_buffer_ != 0)
        visbits_ = fg.use_persistent(string::persistent_buffer_info{
            .name = "meshlet.visbits", .physical = { visbits_buffer_ } });

    // scene.upload is the scene_uniforms component's pass now (declared by the app immediately
    // before this declare, preserving the authoring order).

    // Per-frame reset: zero the stats block and, on demand, the persistent visibility bitfield and
    // last-frame LOD. Authored BEFORE the cull so the graph derives reset->cull; without it the cull
    // accumulates into stats nothing cleared.
    // THREE passes, not one: three producer-consumer stages with three different consumers. As one
    // pass they needed two hand-written memory barriers to order themselves against what came next.
    string::pass_spec reset_lists = fg.pass("meshlet.reset.lists");
    reset_lists.writes(wl_.opaque, string::access::transfer_write)
               .writes(wl_.twosided, string::access::transfer_write)
               .writes(wl_.draw_lod, string::access::transfer_write);
    for (const string::gpu::buffer& c : wl_.cascade)
        reset_lists.writes(c, string::access::transfer_write);
    reset_lists.toggle([this] { return !lists_zeroed_; })
               .transfer([this](string::pass_context& ctx) { record_reset_lists(ctx); });

    fg.pass("meshlet.reset.stats")
      .writes(stats)
      .toggle([this] { return reset_program_ != nullptr && draw_count_ > 0; })
      .compute([this](string::pass_context& ctx) { record_reset_stats(ctx); });

    if (visbits_.valid())
    {
        fg.pass("meshlet.reset.visbits")
          .writes(visbits_, string::access::transfer_write)
          .toggle([this] { return visbits_clear_pending_ && !mesh_cull_frozen_; })
          .transfer([this](string::pass_context& ctx) { record_reset_visbits(ctx); });
    }

    // GPU draw-cull + list expansion. Declared by the meshlet TU, which owns those dispatches.
    declare_cull(fg, stats);
    declare_expand(fg);

    // geometry.phase1. It writes the multisampled depth AND the single-sample hz.depth: two depth
    // writes, one multisampled and one not, which IS the declaration that the first resolves into the
    // second (reverse-Z MIN, the conservative choice for a HiZ occluder). That pairing is what
    // replaced Access::DepthResolve.
    string::pass_spec phase1 = fg.pass("geometry.phase1");
    phase1.color(color)
          // The single-sample resolve target. Declaring BOTH a multisampled and a single-sample
          // colour write IS the declaration that the first resolves into the second — see
          // derive_groups.
          .color(resolve)
          .depth(depth)
          .depth(hiz_depth)
          // The camera lists: the indirect commands, and the records[] the task shader indexes.
          .reads(wl_.opaque, string::access::indirect_read)
          .reads(wl_.opaque, string::access::storage_read, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
          .reads(wl_.twosided, string::access::indirect_read)
          .reads(wl_.twosided, string::access::storage_read, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
          .reads(scene_data)
          // Brief 23: the MESH stage pulls the skin stream + palettes through SceneData. Raster
          // reads default to FRAGMENT (resolve_stages), so the mesh-stage consumption must be
          // said explicitly or it is silently under-barriered.
          .reads(scene_data, string::access::storage_read, VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT)
          .writes(stats);
    // The lit fragments SAMPLE the cascades, GTAO, the IBL products and the froxel list through
    // SceneData's bindless slots — the slots are RESOLVED by scene.upload, but the contents are
    // READ here, at the fragment stage. Declaring the read on the consuming pass is what makes the
    // NEXT frame's producer rewrite derive a write-after-read edge against THIS frame's in-flight
    // fragment work. scene.upload's compute-stage reads cannot stand in for that: a barrier whose
    // source scope is COMPUTE does not wait for fragment sampling still in flight, which is why the
    // shadow term flickered run-to-run — the cascade was rewritten mid-read. (This is the edge the
    // deleted per-frame shadow/GTAO rings used to paper over with triple buffering.)
    const auto lit_reads = [&](string::pass_spec& p) {
        for (const string::gpu::image& c : cascades) p.reads(c);
        p.reads(gtao_ao).reads(env_prefiltered).reads(dfg_lut).reads(ibl_sh).reads(froxels);
    };
    // The registry's content heaps (assets::gpu_view): the task shaders read meshlets for cone/
    // frustum culling, the mesh shaders pull meshlets + remap + triangles + vertices (+ the skin
    // stream when skinned). Declaring them here is what lets the addresses resolve through the
    // pass_context — the allocator escape is gone.
    const auto heap_reads = [&](string::pass_spec& p) {
        const string::gpu::buffer heaps[] = { vertex_buffer_, meshlet_buffer_, meshlet_vertices_,
                                              meshlet_triangles_, skin_buffer_ };
        for (const string::gpu::buffer& h : heaps)
        {
            if (!h.valid()) continue;
            p.reads(h, string::access::storage_read, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT);
            p.reads(h, string::access::storage_read, VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT);
        }
    };
    heap_reads(phase1);
    if (joint_palette_.valid())
        phase1.reads(joint_palette_, string::access::storage_read,
                     VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT);
    lit_reads(phase1);
    if (visbits_.valid())
        phase1.writes(visbits_, string::access::storage_write, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT);
    phase1.toggle([] { return cv_pass_geometry().get(); })
          .raster([this](string::pass_context& ctx) { record_phase1(ctx); });

    declare_hiz(fg, hiz_depth, hiz_pyramid);

    // geometry.phase2 reads the pyramid at the TASK stage — genuinely ambiguous, so it says so — and
    // re-declares the attachments, which is what re-forms the reloaded MSAA group.
    string::pass_spec phase2 = fg.pass("geometry.phase2");
    phase2.color(color)
          .depth(depth)
          .reads(hiz_pyramid, string::access::sampled_read, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
          .reads(wl_.opaque, string::access::indirect_read)
          .reads(wl_.opaque, string::access::storage_read, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
          .reads(wl_.twosided, string::access::indirect_read)
          .reads(wl_.twosided, string::access::storage_read, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
          // Brief 23: phase 2 had NO scene_data read at all (its fragments shade through the
          // same SceneData as phase 1's — the fragment-stage read below via lit_reads' set was
          // carried by phase1 only). Both consumption stages declared: FRAGMENT (shading) and
          // MESH (the skin stream + palette pull).
          .reads(scene_data)
          .reads(scene_data, string::access::storage_read, VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT);
    if (joint_palette_.valid())
        phase2.reads(joint_palette_, string::access::storage_read,
                     VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT);
    lit_reads(phase2);   // phase-2 fragments shade exactly like phase 1's — same consumed set
    heap_reads(phase2);
    if (visbits_.valid())
        phase2.writes(visbits_, string::access::storage_write, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT);
    phase2.toggle([this] { return two_phase_active_ && cv_pass_geometry().get(); })
          .raster([this](string::pass_context& ctx) { record_phase2(ctx); });
}

// Brief 20: the HiZ reduction is one DECLARED PASS PER MIP, not a loop with hand-rolled barriers.
// Mip 0 reduces the resolved scene depth; every later mip reduces the one above it. Because a slice
// is a first-class declaration, `.reads(pyramid.mip(m-1))` and `.writes(pyramid.mip(m))` do not
// collide, so the graph derives the chain — and derives ONLY the chain, rather than serialising the
// whole image the way a single-state tracker had to.
// The chain is authored at kMaxHizMips and each level asks, every frame, whether the CURRENT
// viewport's pyramid actually reaches it. That is the compiled-once answer to a variable-length
// chain: the declaration count is fixed at author time, and the window decides which links survive.
void geometry_pass::declare_hiz(string::frame_graph& fg, string::gpu::image depth,
                                string::gpu::image pyramid)
{
    for (uint32_t m = 0; m < kMaxHizMips; ++m)
    {
        string::pass_spec spec = fg.pass("hiz.mip" + std::to_string(m));
        if (m == 0) spec.reads(depth);
        else        spec.reads(pyramid.mip(m - 1));
        spec.writes(pyramid.mip(m))
            .toggle([this, m] {
                return two_phase_active_ && hiz_program_ != nullptr
                    && m < hiz_mip_count(screen_size);
            })
            .compute([this, m, depth, pyramid](string::pass_context& ctx) {
                record_hiz_mip(ctx, m, depth, pyramid);
            });
    }
}

// One mip's reduction. No barrier: the declaration above is what orders this against the mip before
// it, and against the depth resolve that produced mip 0's source.
void geometry_pass::record_hiz_mip(string::pass_context& ctx, uint32_t m, string::gpu::image depth,
                                   string::gpu::image pyramid)
{
    const HizPyramid& hz = hiz_[ctx.frame_slot];
    // Brief 11 step 2b: the inter-pass barriers this method used to hand-roll are GRAPH-DERIVED now:
    //   - hz.depth DEPTH_ATTACHMENT->SHADER_READ, waiting on the EndRendering MIN-resolve: from
    //     hiz.build's SampledRead(hz.depth) usage + the renderer's post-resolve tracker seed;
    //   - the pyramid UNDEFINED->GENERAL: from hiz.build's StorageImageWrite(pyramid) usage;
    //   - the pyramid GENERAL->SHADER_READ for phase-2's task shader: from geometry.phase2's
    //     SampledRead(pyramid) usage;
    //   - the phase1-read -> phase2-RMW visibility-bitfield serialization: from phase1/phase2's
    //     visbits usages (tracker WAW at the task stage).
    // Only the INTRA-pass per-mip compute->compute barriers below remain — this pass building its own
    // multi-mip resource, a per-mip split the single-state tracker deliberately does not model.
    const ::string::gpu::pipeline& hp = hiz_program_->current();
    ctx.rec.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, hp.pipeline);
    VkDescriptorSet set = descriptor_table_.get_set();
    ctx.rec.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_COMPUTE, hp.pipeline_layout,
                                 0, 1, &set, 0, nullptr);
    const uint32_t dw = std::max(1u, hz.size.x >> m);
    const uint32_t dh = std::max(1u, hz.size.y >> m);
    HizPush hpush{};
    if (m == 0)
    {
        hpush.src_slot = ctx.slot(depth);
        hpush.src_size = glm::uvec2(screen_size.width, screen_size.height);
        hpush.copy_depth = 1;
        hpush.src_level = 0;   // the scene-depth image has a single level
    }
    else
    {
        hpush.src_slot = ctx.slot(pyramid);   // whole-chain view; src_level selects the mip
        hpush.src_size = glm::uvec2(std::max(1u, hz.size.x >> (m - 1)), std::max(1u, hz.size.y >> (m - 1)));
        hpush.copy_depth = 0;
        hpush.src_level = m - 1;
    }
    hpush.dst_slot = ctx.slot(pyramid.mip(m));
    hpush.dst_size = glm::uvec2(dw, dh);
    ctx.rec.push_constants(hp.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(HizPush), &hpush);
    ctx.rec.dispatch((dw + 7) / 8, (dh + 7) / 8, 1);

    // NO barrier here, and none at the end of the chain.
    //
    // The per-mip compute->compute edge is derived from mip m's write against mip m+1's declared
    // read. The pyramid's GENERAL->SHADER_READ handoff to phase 2's task shader is derived from
    // phase 2's declared read. And the hz.depth DEPTH_ATTACHMENT restore is gone: that barrier
    // existed because the tracker forgot everything at the frame boundary, so a resource consumed
    // one frame LATE by GTAO reprojection had to be hand-parked in its resting layout. Tracked
    // state now carries across the boundary, per slot, against that slot's own image — so the
    // cross-frame edge is derived like any other. This was the one exception in the whole renderer
    // that passed the "before the graph exists" test, and it passed it only because nothing
    // modelled it.

    // Brief 09: this slot's hz.depth now holds this frame's resolved phase-1 depth — capture the
    // matrices GTAO will reproject through when it consumes this slot NEXT frame. Rendering uses
    // the LIVE camera even under freeze-cull, so these are the true depth-buffer transforms.
    // Only on the last mip, so it happens once per frame rather than once per dispatch.
    if (m + 1 == hz.mips && ctx.frame_slot < bridge_.depth_history().size())
    {
        DepthHistorySlot& h = bridge_.depth_history()[ctx.frame_slot];
        h.valid = 1;
        h.view = bridge_.frame().view;
        h.view_proj = bridge_.frame().view_proj;
        h.proj = bridge_.frame().view_proj * glm::inverse(bridge_.frame().view);
    }
}

// Brief 04d PHASE 2: recorded INSIDE the reopened MSAA group (which LOADed phase-1's color+depth).
// Renders the disocclusion complement (bit-clear meshlets that the fresh pyramid says are visible),
// sets their bits, then the sorted transparency pass (tests vs the final opaque depth, as before).
void geometry_pass::record_phase2(string::pass_context& ctx)
{
    // Brief 11 step 2: this is now an ALWAYS-scheduled pass (geometry.phase2). In single-pass mode
    // record() already drew the disocclusion-free opaque + probe + transparency, so phase 2 must not
    // run — else it double-draws. Only the two-phase path uses it.
    if (!two_phase_active_ || !meshlet_program_ || !draw_info_mapped_) return;
    record_opaque_phase(ctx, /*phase*/ 2u, pyramid_, scene_data_, stats_);
    // Brief 09b: probe-debug spheres after opaque, depth-tested. Brief 11 M2: transparency moved OUT to
    // the standalone TransparencyPass, which draws last in this same reopened MSAA group (byte-identical).
    // Brief 20: gi.debug is a declared pass; nothing to call from here.
}


const GpuMeshStats& geometry_pass::mesh_stats() const
{
    static const GpuMeshStats none{};
    return uniforms_ != nullptr ? uniforms_->stats_latest() : none;
}

}  // namespace string::render
