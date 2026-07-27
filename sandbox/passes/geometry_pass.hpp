#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/device.hpp>
#include <string/vulkan/render_data.hpp>
#include <string/vulkan/render_pass.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/scene/camera.hpp>
#include <string/gpu/pipeline.hpp>
#include "string/gpu/descriptor_allocator.hpp"
#include "string/gpu/residency_manager.hpp"
#include "string/gpu/resource.hpp"
#include "string/gpu/resource_allocator.hpp"

#include "gltf_loader.hpp"
#include "geometry_streamer.hpp"
#include "texture_streamer.hpp"
#include "lighting_data.hpp"
#include "meshlet_data.hpp"
#include "probe_gi.hpp"
#include "meshlet_builder.hpp"
#include "assetbake/scene_loader.hpp"
#include "debug_cvars.hpp"
#include "geometry/geometry_scene.hpp"
#include "geometry/sky_component.hpp"
#include "geometry/froxel_component.hpp"
#include "geometry/ibl_component.hpp"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <volk.h>

namespace sandbox
{

// FroxelPush moved to geometry/froxel_component.hpp (brief 11 FroxelComponent extraction).
// LightingSettings + the shared scene state moved to geometry/geometry_scene.hpp (brief 11 P2
// GeometryScene extraction); GeometryPass privately inherits GeometryScene below.

// SkyPush moved to geometry/sky_component.hpp (brief 11 SkyComponent extraction).

// Push constant for the task/mesh meshlet draw path (matches Push in shaders/meshlet_mesh.slang;
// std430 push-constant layout — offsets verified against %Push_std430 OpMemberDecorate). 240B; fits
// RDNA3's 256B push budget. Two mat4 (view_proj + cull source), device addresses, then scalars.
// Brief 04c (resolution b): the task shader reads its draw via SV_DrawIndex -> records[] (compacted
// per-surviving-draw {draw_index, lod}), so this carries `records` (not the dead entry worklist).
struct MeshletPush
{
    glm::mat4 view_proj;         // 0
    glm::mat4 cull_view_proj;    // 64
    VkDeviceAddress vertices;    // 128
    VkDeviceAddress meshlets;    // 136
    VkDeviceAddress mverts;      // 144
    VkDeviceAddress mtris;       // 152
    VkDeviceAddress draws;       // 160
    VkDeviceAddress scene;       // 168
    VkDeviceAddress stats;       // 176
    VkDeviceAddress records;     // 184 (brief 04c: compacted {draw_index, lod} per indirect draw)
    glm::vec3 camera_pos;        // 192
    float _pad_cp;               // 204
    uint32_t debug_view;         // 208
    uint32_t hiz_slot;           // 212
    uint32_t hiz_mips;           // 216
    uint32_t _pad_hiz;           // 220 (std430: uvec2 hiz_size is 8-aligned -> pad to 224)
    glm::uvec2 hiz_size;         // 224
    uint32_t blend_pass;         // 232 (brief 04: 1 = transparency pass -> fragment returns base.a)
    // Brief 04d two-phase occlusion. phase: 0 = single-pass (legacy, HiZ-off / transparency), 1 =
    // phase-1 (render meshlets whose bit is SET — visible last frame; no HiZ), 2 = phase-2 (render
    // the complement whose bit is CLEAR + HiZ-visible, and update bits). `bitfield` is the persistent
    // per-meshlet visibility bitfield (1 bit per global meshlet). freeze_bits: 1 = do NOT mutate bits
    // (freeze). The bit index is the meshlet's GLOBAL meshlet id (draw's lod meshlet_offset + local).
    uint32_t phase;              // 236
    VkDeviceAddress bitfield;    // 240 (8-aligned; persistent visibility bits)
    uint32_t freeze_bits;        // 248
    uint32_t _pad_end2;          // 252 (pad to 256)
};
static_assert(offsetof(MeshletPush, records) == 184);
static_assert(offsetof(MeshletPush, camera_pos) == 192);
static_assert(offsetof(MeshletPush, debug_view) == 208);
static_assert(offsetof(MeshletPush, hiz_size) == 224);
static_assert(offsetof(MeshletPush, blend_pass) == 232);
static_assert(offsetof(MeshletPush, phase) == 236);
static_assert(offsetof(MeshletPush, bitfield) == 240);
static_assert(offsetof(MeshletPush, freeze_bits) == 248);
static_assert(sizeof(MeshletPush) == 256);

// Push for the depth-only meshlet shadow path (matches Push in shaders/meshlet_shadow.slang).
// Brief 04c (resolution b): reads its draw via SV_DrawIndex -> records[] (compacted per-cascade).
struct MeshletShadowPush
{
    glm::mat4 light_view_proj;   // 0
    VkDeviceAddress vertices;    // 64
    VkDeviceAddress meshlets;    // 72
    VkDeviceAddress mverts;      // 80
    VkDeviceAddress mtris;       // 88
    VkDeviceAddress draws;       // 96
    VkDeviceAddress records;     // 104 (brief 04c: compacted {draw_index, lod} per indirect draw)
    uint32_t _pad0;              // 112
    uint32_t _pad1;              // 116
};
static_assert(offsetof(MeshletShadowPush, records) == 104);
static_assert(sizeof(MeshletShadowPush) == 120);

// Push for the GPU draw-cull compute (matches Push in shaders/meshlet_draw_cull.slang; std430).
struct DrawCullPush
{
    glm::mat4 cull_view_proj;          // 0   draw-level frustum source (frozen-aware; C disables)
    VkDeviceAddress draws;             // 64
    VkDeviceAddress counts;            // 72  brief 04c: per-draw one-sided meshlet count
    VkDeviceAddress counts_twosided;   // 80  brief 04c: per-draw two-sided meshlet count
    VkDeviceAddress counts_shadow;     // 88  brief 04c: per-draw resident-only shadow count
    VkDeviceAddress draw_lod;          // 96  brief 04c: per-draw selected LOD (shared by lists)
    VkDeviceAddress stats;             // 104
    uint32_t draw_count;               // 112
    uint32_t lod_enabled;              // 116
    uint32_t frustum_cull;             // 120 (0 = C toggle off -> pass everything)
    uint32_t stats_lod;                // 124 (1 = write the LOD histogram; camera pass only)
    glm::vec3 camera_pos;              // 128 (16-aligned; frozen-aware — drives LOD select)
    float lod_error_px;                // 140
    float focal;                       // 144
    uint32_t shadow_mode;              // 148 (brief 04 M4: 1 -> draw-cull shadow list vs cascade sphere)
    float shadow_radius;               // 152 (cascade bounding-sphere radius, world units)
    uint32_t force_lod0;               // 156 (DEAD since brief 04d deleted the LOD0 HiZ prepass; always
                                       //      written 0 — kept as a padding slot to preserve push offsets)
    int32_t isolate_draw;              // 160 (brief 06: >=0 -> only this draw survives; -1 = off).
                                       //     Repurposes a former pad slot, so all offsets/size hold.
    uint32_t _pad3b;                   // 164
    uint32_t _pad3c;                   // 168
    uint32_t _pad3d;                   // 172
    glm::vec3 shadow_center;           // 176 (cascade bounding-sphere centre, world; 16-aligned)
    float _pad4;                       // 188
};
static_assert(offsetof(DrawCullPush, draws) == 64);
static_assert(offsetof(DrawCullPush, counts_twosided) == 80);
static_assert(offsetof(DrawCullPush, counts_shadow) == 88);
static_assert(offsetof(DrawCullPush, draw_lod) == 96);
static_assert(offsetof(DrawCullPush, camera_pos) == 128);
static_assert(offsetof(DrawCullPush, lod_error_px) == 140);
static_assert(offsetof(DrawCullPush, shadow_mode) == 148);
static_assert(offsetof(DrawCullPush, force_lod0) == 156);
static_assert(offsetof(DrawCullPush, shadow_center) == 176);
static_assert(sizeof(DrawCullPush) == 192);

// Push for the brief-04c compaction/expansion compute (matches Push in shaders/meshlet_expand.slang).
// The scan compacts SURVIVING draws (counts[d] != 0) into a dense array of per-draw indirect commands
// + parallel records, in ascending draw-index order, and writes the surviving-draw count.
struct ExpandPush
{
    VkDeviceAddress counts;      // 0   in : per-draw meshlet count (stable slot d), 0 = culled
    VkDeviceAddress offsets;     // 8   scratch: per-draw block-local compact index
    VkDeviceAddress block_sums;  // 16  scratch: per-block surviving total -> block base
    VkDeviceAddress draw_lod;    // 24  in : per-draw selected LOD (fill copies it into records[])
    VkDeviceAddress commands;    // 32  out: dense VkDrawMeshTasksIndirectCommandEXT {ceil(mcount/32),1,1}
    VkDeviceAddress records;     // 40  out: dense parallel {draw_index, lod}
    VkDeviceAddress count;       // 48  out: surviving-draw count (indirect draw count)
    uint32_t draw_count;         // 56
    uint32_t block_count;        // 60  ceil(draw_count / kScanBlock)
    uint32_t max_draws;          // 64  commands[]/records[] capacity (overflow guard)
    uint32_t _pad;               // 68
};
static_assert(offsetof(ExpandPush, commands) == 32);
static_assert(offsetof(ExpandPush, records) == 40);
static_assert(offsetof(ExpandPush, count) == 48);
static_assert(offsetof(ExpandPush, draw_count) == 56);
static_assert(offsetof(ExpandPush, max_draws) == 64);
static_assert(sizeof(ExpandPush) == 72);

// Push for the HiZ pyramid downsample (matches Push in shaders/hiz_build.slang).
struct HizPush
{
    uint32_t src_slot;
    uint32_t dst_slot;
    glm::uvec2 dst_size;
    glm::uvec2 src_size;
    uint32_t copy_depth;
    uint32_t src_level;   // mip level of `src_slot` to sample (pyramid->pyramid: the level being reduced)
};

// Push for the brief-09 GTAO chain (matches Push in shaders/gtao.slang; std430: mat4s at 0/64,
// scalars from 128, uint2s 8-aligned at 136/144).
struct GtaoPush
{
    glm::mat4 view;         // 0    prev-frame world->view
    glm::mat4 inv_proj;     // 64   prev-frame inverse projection
    uint32_t depth_slot;    // 128  prev hz.depth sampled slot
    uint32_t dst_slot;      // 132
    glm::uvec2 dst_size;    // 136
    glm::uvec2 depth_size;  // 144
    float radius;           // 152
    float proj00;           // 156
    float proj11;           // 160
    uint32_t src_slot;      // 164  denoise input (raw AO)
    uint32_t _pad0;         // 168
    uint32_t _pad1;         // 172
};
static_assert(offsetof(GtaoPush, depth_slot) == 128);
static_assert(offsetof(GtaoPush, dst_size) == 136);
static_assert(offsetof(GtaoPush, radius) == 152);
static_assert(sizeof(GtaoPush) == 176);

// Push for the per-frame stats reset (matches Push in shaders/meshlet_reset.slang).
struct ResetPush
{
    VkDeviceAddress stats;
    uint32_t stats_words;
    uint32_t _pad;
};

// IblPush moved to geometry/ibl_component.hpp (brief 11 IblComponent extraction).

// Renders a whole glTF model: uploads its shared vertex/index buffers plus every texture (each
// into a bindless slot), then issues one indexed draw per node-instanced primitive, pushing the
// primitive's transform and material inline. Content-agnostic — the model path is supplied by
// the application, resolved under context.resources_path.
class GeometryPass final : public String::Pass, private GeometryScene
{
    string::gpu::device& device_;
    string::gpu::resource_allocator& allocator_;
    string::gpu::descriptor_table& descriptor_table_;
    String::InputMap& input_map_;
    // Froxel light binning: extracted to FroxelComponent froxel_ (declared below, brief 11).

    string::gpu::resource_id vertex_buffer_;
    // Brief 11 step 2: the scene MSAA color/depth sentinels (from engine_context), cached so the
    // geometry.phase2 sub-pass declares the SAME attachments to form the reopened MSAA group.
    string::gpu::resource_id color_target_ = 0;
    string::gpu::resource_id depth_target_ = 0;
    // draw_count_ / frames_in_flight_ moved to GeometryScene (brief 11 P2).
    // Brief 04e M2: number of fixed usages declared at construction; update() truncates back to
    // this count and re-appends the current frame slot's buffer usages (worklists, froxels,
    // visibility bitfield) so the frame graph sees per-frame-true declarations.
    std::size_t static_usage_count_ = 0;

    // Geometry residency streaming: per-draw vertex/index ranges suballocate into heaps SMALLER than
    // the whole model as draws enter the view frustum, and are freed (reclaimed) on eviction. A
    // not-yet-resident draw stays hidden (DrawInfo.resident 0). Capping resident geometry below the full
    // model is what exercises reclaim; raise toward 100 for a VRAM-fitting scene with no pop-in.
    static constexpr std::uint64_t kGeometryResidentPercent = 100;
    static constexpr VkDeviceSize kGeometryStreamPerFrame = 32ull * 1024 * 1024;
    // Per-model budget (heap capacity), so this is a unique_ptr built once sizes are known.
    std::unique_ptr<string::gpu::residency_manager> geometry_residency_;
    std::unique_ptr<GeometryStreamer> geometry_streamer_;
    // Only stream per-frame when the model doesn't fit the budget (< 100%). When it fits, all
    // geometry is uploaded up front (no per-frame streaming/eviction cost) — streaming a scene that
    // fits in VRAM just adds startup lag for no benefit.
    bool geometry_streaming_ = false;

    // Per glTF-image backing resource + its bindless slot (index-aligned with the loaded
    // model's textures, so a material's texture index maps straight to a slot).
    std::vector<string::gpu::resource_id> texture_images_;
    std::vector<uint32_t> texture_slots_;
    // 1x1 white fallback, used for draws whose material has no base-color texture (the
    // base-color factor still tints it) — also the metallic-roughness fallback (white .g/.b = 1,
    // so metallic/roughness reduce to the scalar factors).
    string::gpu::resource_id white_image_;
    uint32_t white_slot_ = 0;
    // 1x1 flat-normal fallback (tangent-space +Z = RGBA 128,128,255), for draws with no normal map:
    // sampling it yields the geometric normal, so the shader needs no branch.
    string::gpu::resource_id flat_normal_image_;
    uint32_t flat_normal_slot_ = 0;

    // Cascaded shadow maps. A depth-only prepass (in record_compute) renders the scene from the sun's
    // orthographic view into cascade_count per-frame-in-flight D32 images (one per cascade), sampled
    // by the lit fragment shader with 5x5 PCF. Cascades are stabilized (texel-snapped) and refit each
    // frame because the sun and camera both move (dynamic time-of-day).
    // settings_/sun_dir_/cascade_*/time_of_day_/sun_animate_/sky_*/sun_*/furnace_/lookdev_ moved to
    // GeometryScene (brief 11 P2).
    // Fullscreen procedural sky moved OUT to its own registered SkyPass (brief 11 step 3) — it owns the
    // SkyComponent + reads GeometryScene, drawn as the first pass of the scene MSAA group.

    // Brief 07/11-step-3 dynamic sky IBL is its own IblPass (prepass compute) now; the component it owns
    // is published into GeometryScene (scene().ibl) — GeometryPass reads sh_address/env/dfg slots for
    // SceneData, and probe GI reads the SH. The shared sun/sky colours + furnace flag are in GeometryScene.

    // --- Brief 09b: relightable irradiance probe volume (DDGI-style) -----------------------------
    // A uniform probe grid fit to the scene AABB. Static per-probe capture G-buffer (albedo, normal,
    // distance) + visibility (mean/mean^2 distance) octahedral atlases, rasterized once at load; a
    // compute pass RELIGHTS them into an octahedral irradiance atlas whenever the sun moves (like the
    // sky IBL). Shading samples the irradiance atlas (Chebyshev-visibility-gated 8-probe trilinear)
    // for the ambient DIFFUSE term, falling back to the sky-SH ambient outside the volume. See
    // probe_gi.hpp / probe_common.slang for the layout + math. r.gi toggles the whole feature.
    // M1 capture: a single reused per-probe cube G-buffer (albedo + normal/dist + depth), rasterized
    // per cube face then collapsed into this probe's octahedral atlases. K probes/frame, amortized.
    static constexpr uint32_t kProbeCubeFace = 32;    // cube face size (cheap; > vis tile so no aliasing)
    // Capture is a full-scene mesh render x6 faces x this many probes PER FRAME — the dominant
    // load-time cost. 1 face/frame keeps every frame under the hitch budget (capture spreads over
    // ~faces*probes frames; the scene is playable throughout). See record_probe_capture.
    // Brief 09b (fast capture): all 6 cube faces render in ONE multiview pass, so the amortization
    // unit is PROBES/frame (not faces): with the per-face CPU loop gone, a full probe is one
    // vkCmdDrawMeshTasksEXT. ~10+ probes/frame finishes a ~250-probe bake in a couple of seconds.
    static constexpr uint32_t kProbeCubeViewMask = 0x3Fu;   // 6-face multiview mask
    // Probes captured per frame (amortized). Each probe is one multiview dispatch over the coarse-LOD
    // meshlet domain (distance + face culled on GPU), so this bounds the per-frame bake burst. 2 keeps
    // it under the hitch threshold; a ~750-probe bake still finishes in a few seconds.
    static constexpr uint32_t kProbesPerFrame = 2;
    ProbeVolume probe_volume_{};
    string::gpu::resource_id probe_cube_albedo_ = 0;  // cube RGBA16F: captured base color
    string::gpu::resource_id probe_cube_nd_ = 0;      // cube RGBA16F: world normal.xyz + linear dist(w)
    string::gpu::resource_id probe_cube_depth_ = 0;   // cube D32: nearest-surface depth (per face)
    uint32_t probe_cube_albedo_sample_slot_ = 0;      // SamplerCube (collapse read)
    uint32_t probe_cube_nd_sample_slot_ = 0;
    // 6-layer 2D_ARRAY views over the whole cube — the multiview render pass targets all 6 layers in
    // one BeginRendering (SV_ViewID selects the layer/face).
    VkImageView probe_cube_albedo_array_view_ = VK_NULL_HANDLE;
    VkImageView probe_cube_nd_array_view_ = VK_NULL_HANDLE;
    VkImageView probe_cube_depth_array_view_ = VK_NULL_HANDLE;
    string::gpu::resource_id probe_active_ = 0;       // per-probe activation (uint: 1 active / 0 inside)
    string::gpu::resource_id probe_offset_ = 0;       // per-probe relocation offset (float4: xyz world)
    string::gpu::resource_id probe_meshlet_draw_ = 0; // global LOD0-meshlet-id -> owning draw index (uint)
    uint32_t probe_total_lod0_meshlets_ = 0;          // total LOD0 meshlets (flat capture dispatch domain)
    float probe_cull_far_ = 0.0f;                     // capture distance-cull radius (bounded ~= few cells)
    string::gpu::shader_program* probe_capture_raster_program_ = nullptr;  // cube-face G-buffer raster
    string::gpu::shader_program* probe_collapse_program_ = nullptr;        // cube -> octa atlases + classify
    uint32_t probe_capture_cursor_ = 0;               // probe currently being captured (amortization)
    bool probe_cube_layouts_initialized_ = false;     // cube G-buffer images: UNDEFINED -> first use
    double probe_capture_ms_ = 0.0;                   // accumulated capture wall time (log on complete)
    string::gpu::resource_id probe_irrad_ = 0;        // octa irradiance atlas (RGBA16F, relit)
    string::gpu::resource_id probe_cap_gbuf_ = 0;     // octa capture normal.xyz + hit distance (w)
    string::gpu::resource_id probe_cap_albedo_ = 0;   // octa capture albedo.rgb
    string::gpu::resource_id probe_vis_ = 0;          // octa visibility mean/mean^2 (RG16F used)
    uint32_t probe_irrad_sample_slot_ = 0;            // Sampler2D (shading + debug + bounce)
    uint32_t probe_irrad_storage_slot_ = 0;           // RWTexture2D (relight write)
    uint32_t probe_cap_gbuf_sample_slot_ = 0;
    uint32_t probe_cap_gbuf_storage_slot_ = 0;
    uint32_t probe_cap_albedo_sample_slot_ = 0;
    uint32_t probe_cap_albedo_storage_slot_ = 0;
    uint32_t probe_vis_sample_slot_ = 0;
    uint32_t probe_vis_storage_slot_ = 0;
    VkSampler probe_sampler_ = VK_NULL_HANDLE;        // linear clamp (atlas bilinear w/ gutter)
    string::gpu::shader_program* probe_clear_program_ = nullptr;   // capture: clear/init (M0), raster (M1)
    string::gpu::shader_program* probe_relight_program_ = nullptr; // relight + convolve
    string::gpu::shader_program* probe_debug_program_ = nullptr;   // instanced debug spheres
    bool probe_layouts_initialized_ = false;  // atlases moved UNDEFINED -> GENERAL once
    bool probe_captured_ = false;             // static capture (clear/vis) recorded once
    bool probe_primed_ = false;               // at least one FULL relight pass has completed
    bool probe_relight_pending_ = false;      // armed passes remain -> record_compute runs relight
    // M2 amortized relight: K probes per frame, round-robin cursor. A sun trigger arms N converge
    // passes: each full wrap propagates the bounce one step further (infinite bounce) and the
    // hysteresis EMA settles (h^N residual), then relight goes idle until the sun moves (~0 static).
    static constexpr uint32_t kRelightProbesPerFrame = 128;
    static constexpr uint32_t kRelightConvergePasses = 32;
    uint32_t probe_relight_cursor_ = 0;       // next probe to relight within the current pass
    uint32_t probe_relight_passes_left_ = 0;  // full passes remaining before relight goes idle
    glm::vec3 probe_relit_sun_dir_{ 0.0f };
    uint32_t probe_debug_mode_ = 0;           // r.gi.probe_debug snapshot for this frame's record()
    void fit_probe_volume();                  // grid fit to the scene AABB (spacing CVar)
    void create_probe_resources(String::engine_context& context);
    void record_probe_capture(string::gpu::command_recorder& recorder);   // static capture (once): init/raster + visibility
    void record_probe_relight(string::gpu::command_recorder& recorder, uint16_t current_frame);  // relight -> irradiance
    void record_probe_debug(string::gpu::command_recorder& recorder);     // instanced probe-debug spheres (into scene)
    // Shadow depth images: [frame_in_flight][cascade]. One D32 map per cascade per frame in flight,
    // so frame N+1's shadow render doesn't race N's sample and each cascade has its own map.
    std::vector<std::array<string::gpu::resource_id, kMaxCascades>> shadow_images_;
    std::vector<std::array<uint32_t, kMaxCascades>> shadow_slots_;
    VkSampler shadow_sampler_ = VK_NULL_HANDLE;             // nearest + clamp, for manual PCF
    // Recompute the per-cascade stabilized ortho fits from the live camera + sun (called per frame).
    void compute_cascades();

    // --- Forward+ lighting: scene data + local lights + froxels --------------------------------
    // scene_buffers_/scene_mapped_ + lights_/light_buffers_/light_mapped_ moved to GeometryScene
    // (brief 11 P2).
    // Forward+ froxel light-binning is its own FroxelPass now (brief 11 step 3); the component it owns
    // is published into GeometryScene (scene().froxel) so SceneData can read its grid dims + address.
    // Light stress scene: hundreds of moving colored lights orbiting over the model (test bed).
    void build_light_stress_scene(const glm::vec3& aabb_min, const glm::vec3& aabb_max);
    void animate_lights(float delta_time);
    struct LightAnim { glm::vec3 center; float radius; float speed; float phase; float height; };
    std::vector<LightAnim> light_anim_;
    // scene_aabb_min_/scene_aabb_max_ + lights_enabled_ moved to GeometryScene (brief 11 P2/step 3).
    bool froxel_heatmap_ = false;  // H toggles the froxel light-count heatmap

    // Texture residency streaming: the streamer (a residency_provider) owns each texture's mip
    // levels and adjustable-minLod sampler; the manager drives what streams in per frame. Only
    // cooked KTX2 textures stream — stb-decoded fallbacks upload whole as before. Budget is large
    // (no physical VRAM reclaim yet — see TextureStreamer), so all wanted detail streams in.
    static constexpr VkDeviceSize kTextureBudget = 6ull * 1024 * 1024 * 1024;
    // Cap new streaming per frame so the coarse scene appears instantly and sharpens over ~a second,
    // rather than stalling frame 0 on the whole fine-mip upload.
    static constexpr VkDeviceSize kTextureStreamPerFrame = 64ull * 1024 * 1024;
    // Coarse-mip LOD floor: the coarsest resident mip every streamed texture is pinned to is roughly
    // this wide, so the whole scene renders blurry-but-real up front and only visible surfaces pull
    // finer mips (screen-coverage feedback in update()).
    static constexpr std::uint32_t kCoarseFloorPixels = 64;
    string::gpu::residency_manager residency_;
    std::unique_ptr<TextureStreamer> texture_streamer_;
    std::vector<string::gpu::resource_id> streamed_textures_;
    // Per glTF texture (index-aligned with texture_images_): the info the coverage heuristic needs
    // to pick a desired mip. levels == 0 marks a non-streamed (stb) texture the feedback skips.
    struct TextureLod
    {
        std::uint32_t levels = 0;         // total mip levels
        std::uint32_t base_extent = 0;    // max(width, height) of mip 0
        std::uint32_t coarse_detail = 0;  // pinned coarse-tail detail (never wanted below this)
    };
    std::vector<TextureLod> texture_lod_;
    // Scratch reused each frame: the finest detail any visible draw wants per texture (starts at the
    // coarse floor, raised by coverage), issued as one want() per texture after the draw loop.
    std::vector<std::uint32_t> frame_desired_detail_;
    uint64_t stream_frame_ = 0;
    bool logged_full_resident_ = false;

    // materials_/draws_ + camera_ moved to GeometryScene (brief 11 P2).

    // Debug: toggle GPU frustum culling off entirely (C) — for isolating culling from geometry.
    bool cull_enabled_ = true;

    // --- Brief 03: task/mesh meshlet pipeline --------------------------------------------------
    // Built at load from the flattened geometry: the GPU-side meshlet/vertex/triangle heaps + the
    // per-draw DrawInfo table. Metadata buffers are small and always resident; only the vertex/index
    // heaps stream (DrawInfo.resident is the streaming gate).
    // Brief 04b spatial-chunking budget: draws whose LOD0 meshlet count exceeds this are split into
    // tight-bounds chunks at COOK time (the loader selects the cooked variant baked with this budget).
    // Brief 04c M4: chunking FINALIZED OFF by default (r.chunk.budget default 0) — under resolution
    // (b)'s per-draw command boundaries the chunk fan-out is not free (measured +~1.8 ms interior).
    // This constant is the CHUNKED variant's budget, used only when r.chunk.budget is set non-zero for
    // A/B; kept here so the loader + cooked-file naming agree.
    static constexpr uint32_t kChunkMaxMeshlets = 1024;
    // meshlet_model_/meshlet_buffer_/meshlet_vertices_/meshlet_triangles_/draw_info_buffer_/
    // draw_info_mapped_ + stats_buffers_/stats_readback_/stats_latest_ moved to GeometryScene
    // (brief 11 P2).
    // Mesh pipelines (hot-reloadable via the registry).
    string::gpu::shader_program* meshlet_program_ = nullptr;
    string::gpu::shader_program* meshlet_twosided_program_ = nullptr;  // brief 04: CULL_NONE variant
    string::gpu::shader_program* meshlet_transparent_program_ = nullptr; // brief 04: blend, no depth write
    string::gpu::shader_program* meshlet_shadow_program_ = nullptr;
    string::gpu::shader_program* hiz_program_ = nullptr;
    string::gpu::shader_program* reset_program_ = nullptr;
    // Brief 04c: two-phase GPU cull for the global meshlet worklists. (1) draw-cull compute writes each
    // draw's surviving meshlet COUNT + selected LOD at a stable slot; (2) expand compute prefix-scans
    // the counts (order-stable, no atomics) into a compacted {draw_index, meshlet_local} worklist +
    // the indirect groupCount for a SINGLE vkCmdDrawMeshTasksIndirectEXT per list. Draw count no
    // longer drives dispatch cost.
    string::gpu::shader_program* draw_cull_program_ = nullptr;
    string::gpu::shader_program* expand_scan_blocks_program_ = nullptr;
    string::gpu::shader_program* expand_scan_carry_program_ = nullptr;
    string::gpu::shader_program* expand_fill_program_ = nullptr;
    // A worklist buffer packs, at 16B-aligned regions: counts[max_draws], offsets[max_draws],
    // block_sums[max_blocks] (scan scratch), then the COMPACTED draw list — commands[max_draws]
    // (12B VkDrawMeshTasksIndirectCommandEXT), records[max_draws] (8B {draw_index, lod}), and the
    // surviving-draw count word (for vkCmdDrawMeshTasksIndirectCountEXT). One command per surviving
    // draw (empty slots compacted out): removes the fan-out cost while keeping command ordering.
    // Brief 04e M3: worklists are SCRATCH regions now — `offset` into the renderer's per-frame-
    // slot scratch arena, reserved once at load (same offset in every slot's buffer); `buffer`
    // is the slot's scratch buffer, bound lazily in update() once the arena is materialized.
    struct Worklist { string::gpu::resource_id buffer = 0; VkDeviceSize offset = 0; };
    // scratch_/scratch_bound_ moved to GeometryScene (brief 11 P2).
    // Per-frame-in-flight (the GPU may still read last frame's list). Camera lists: opaque one-sided +
    // two-sided (brief 04d: shared by phase 1 and phase 2; the task shader partitions per phase — the
    // old forced-LOD0 depth-prepass lists are deleted along with the prepass itself).
    std::vector<Worklist> wl_opaque_;
    std::vector<Worklist> wl_twosided_;
    // Brief 04 M4: one shadow worklist PER CASCADE per frame (resident-only; per-cascade light-sphere
    // reject in the draw phase — off-camera casters that reach the cascade still cast in).
    std::vector<std::array<Worklist, kMaxCascades>> wl_shadow_;
    // Shared per-draw selected LOD (one per frame; every camera-derived list + shadows read it).
    // Shared per-draw selected LOD: a scratch region (offset into each slot's arena buffer).
    VkDeviceSize draw_lod_off_ = 0;
    VkDeviceAddress draw_lod_address(uint16_t frame) const
    {
        return scratch_->address(frame) + draw_lod_off_;
    }
    uint32_t cull_max_draws_ = 0;
    uint32_t cull_max_blocks_ = 0;      // ceil(max_draws / kScanBlock)
    VkDeviceSize wl_offsets_off_ = 0;   // byte offset of offsets[] within a worklist buffer
    VkDeviceSize wl_blocksums_off_ = 0; // byte offset of block_sums[]
    VkDeviceSize wl_commands_off_ = 0;  // byte offset of the compacted indirect commands[] (12B each)
    VkDeviceSize wl_records_off_ = 0;   // byte offset of the parallel records[] (8B each)
    VkDeviceSize wl_count_off_ = 0;     // byte offset of the surviving-draw count word
    // Draw phase for one camera frame (writes opaque+twosided counts + draw_lod), or a shadow cascade
    // (cascade>=0: resident-only counts vs the cascade sphere).
    void record_draw_cull(string::gpu::command_recorder& recorder, uint16_t current_frame, int cascade = -1);
    // Compaction phase for one worklist (scan + fill): compacts surviving draws into commands[]+records[]
    // + count, ready for one vkCmdDrawMeshTasksIndirectCountEXT. `draw_lod` is the per-draw selected-LOD
    // buffer the records[] inherit (camera-selected for camera+shadow lists, zeroed for the LOD0 prepass).
    void record_expand(string::gpu::command_recorder& recorder, const Worklist& wl, VkDeviceAddress draw_lod);

    // Brief 04 sorted transparency pass. BLEND draws are excluded from the opaque lists (GPU compute)
    // and rendered here after opaque + sky: depth-tested vs opaque depth, NO depth write, alpha-blended,
    // one CPU back-to-front-sorted COMPACTED per-draw command list (commands[] then records[] then
    // count in a single host-visible buffer per frame — same shape as the GPU-compacted lists, so the
    // same task/mesh consumer + vkCmdDrawMeshTasksIndirectCountEXT draws it). Draw-order (back-to-front)
    // is preserved because the CPU writes commands/records in sorted order. Counts are small (no OIT).
    std::vector<string::gpu::resource_id> transp_buffers_;   // per-frame host-visible {commands[], records[], count}
    std::vector<uint32_t*> transp_mapped_;
    VkDeviceSize transp_commands_off_ = 0;
    VkDeviceSize transp_records_off_ = 0;
    VkDeviceSize transp_count_off_ = 0;
    std::vector<uint32_t> blend_draw_indices_;               // draw indices flagged BLEND (built at load)
    uint32_t build_transparency_list(uint16_t current_frame);  // sorts + fills the list; returns count
    void record_transparency(string::gpu::command_recorder& recorder, uint16_t current_frame, uint32_t count);

    // HiZ depth pyramid (R32F mip chain), one per frame in flight. Built each frame from the scene
    // depth via a MIN reduce (reverse-Z: min = farthest). Sampled by the pass-2 task shader.
    struct HizPyramid
    {
        string::gpu::resource_id image = 0;            // full mip chain, sampled slot
        uint32_t sample_slot = 0;                      // bindless combined-image-sampler (whole chain)
        std::vector<VkImageView> mip_views;            // per-mip storage views
        std::vector<uint32_t> mip_storage_slots;       // per-mip STORAGE_IMAGE bindless slots
        uint32_t mips = 0;
        glm::uvec2 size{ 0, 0 };
        // Single-sample depth the pyramid's mip 0 reduces from. The scene's shared depth target is
        // 4x MSAA (can't be sampled with a plain Sampler2D), so brief 04d MIN-resolves the phase-1
        // MSAA depth into this single-sample image (the renderer's depth-resolve target, filled at
        // phase-1's EndRendering), and record_between() builds the pyramid from it.
        string::gpu::resource_id depth = 0;
        uint32_t depth_slot = 0;                       // sampled slot of the min-resolved phase-1 depth
    };
    std::vector<HizPyramid> hiz_;
    // Brief 16 M7 (#1): the hiz depth + pyramid image RINGS are registry-owned PerFrame images now
    // (the registry is the allocation authority — create/recreate/free). The pass keeps the technique
    // descriptor wiring (per-mip views + sampled/storage bindless slots) over the resolved physicals.
    string::gpu::image hiz_depth_ring_;
    string::gpu::image hiz_pyramid_ring_;
    VkSampler hiz_sampler_ = VK_NULL_HANDLE;
    uint32_t hiz_screen_w_ = 0, hiz_screen_h_ = 0;
    void ensure_hiz(uint16_t current_frame);

    // --- Brief 09: half-res GTAO with bent normals ----------------------------------------------
    // Computed at the top of record_compute from the PREVIOUS frame slot's min-resolved depth
    // (hz.depth) under that frame's matrices; consumed by this frame's lit fragments through
    // SceneData.prev_view_proj reprojection (exact for the static scene; 1-frame-late AO, zero
    // temporal accumulation so nothing can ghost). raw -> denoise -> per-slot final (RGBA8:
    // rgb bent world normal, a visibility). Requires the two-phase HiZ depth; auto-off otherwise.
    // Brief 16 M7 (#1): the gtao raw (shared, 1-deep) + final (per-frame) image rings are registry-
    // owned PerFrame images now; the pass keeps the sampled/storage bindless-slot wiring.
    string::gpu::image gtao_raw_ring_;
    string::gpu::image gtao_final_ring_;
    string::gpu::resource_id gtao_raw_ = 0;
    uint32_t gtao_raw_sampled_slot_ = 0;
    uint32_t gtao_raw_storage_slot_ = UINT32_MAX;
    bool gtao_raw_initialized_ = false;      // UNDEFINED -> first-layout transition done
    std::vector<string::gpu::resource_id> gtao_final_;
    std::vector<uint32_t> gtao_final_sampled_slots_;
    std::vector<uint32_t> gtao_final_storage_slots_;
    std::vector<uint8_t> gtao_final_ready_;  // slot image holds data + sits in SHADER_READ_ONLY
    glm::uvec2 gtao_size_{ 0, 0 };
    VkSampler gtao_sampler_ = VK_NULL_HANDLE;   // linear clamp (half-res AO upsampled in the shader)
    string::gpu::shader_program* gtao_program_ = nullptr;
    string::gpu::shader_program* gtao_denoise_program_ = nullptr;
    // Per-slot camera state captured when that slot's depth was resolved (reproject targets).
    std::vector<uint8_t> hz_depth_valid_;    // slot's hz.depth holds a resolved frame
    std::vector<glm::mat4> gtao_slot_view_;
    std::vector<glm::mat4> gtao_slot_proj_;
    std::vector<glm::mat4> gtao_slot_view_proj_;
    bool gtao_runs_this_frame_ = false;      // decided in update() (SceneData needs it pre-record)
    void ensure_gtao();
    void record_gtao(string::gpu::command_recorder& recorder, uint16_t current_frame);

    // Brief 06: address of the renderer's Tracy GPU context (from engine_context), for finer per-stage
    // GPU zones inside record_compute/record. Null when the renderer exposes none; the zone macros
    // are no-ops without -Dtracy regardless. Read live at record time (the ctx is created after the
    // pass is built). Helper below turns the double-indirection into the value the macros want.
    STRING_PROFILE_GPU_CONTEXT_TYPE* gpu_profiler_ctx_ = nullptr;
    STRING_PROFILE_GPU_CONTEXT_TYPE gpu_ctx() const
    {
        STRING_PROFILE_GPU_CONTEXT_TYPE none{};
        return gpu_profiler_ctx_ ? *gpu_profiler_ctx_ : none;
    }

    // Runtime state.
    bool meshlet_readback_pending_ = false;  // STRING_MESHLET_READBACK: GPU-vs-CPU memcmp at frame 50
    bool hiz_enabled_ = true;        // O toggles two-pass HiZ occlusion on the meshlet path
    bool lod_enabled_ = true;        // (LOD select on by default)
    int debug_view_ = 0;             // 0 none, 1 meshlet colour, 2 LOD colour, 3 occlusion reject (V cycles)
    bool mesh_cull_frozen_ = false;  // meshlet freeze-cull (F keybind)
    glm::mat4 mesh_frozen_view_proj_{ 1.0f };
    // dbg.orbit motion lever: continuously sways the camera around a captured base pose so headless
    // captures exercise per-frame disocclusion (the two-phase interleaved phase-1/phase-2 path).
    // Base pose is latched on the first orbiting frame; the sway is applied AFTER camera_.update.
    bool orbit_base_latched_ = false;
    glm::vec3 orbit_base_pos_{ 0.0f };
    float orbit_base_yaw_ = 0.0f;
    float orbit_base_pitch_ = 0.0f;
    float orbit_phase_ = 0.0f;       // accumulated angle (rad)
    // Freeze-cull must freeze EVERY camera-dependent cull input, not just the frustum planes:
    // cone culling + the HiZ nearest-point test + LOD select all use the eye position, and the
    // HiZ prepass/test use the view-projection. A partial freeze mixes frozen and live inputs
    // and produces bogus culling as the real camera moves away.
    glm::vec3 mesh_frozen_camera_pos_{ 0.0f };
    // Crowd stress scene (K): the base draws duplicated across a grid with varied transforms, to
    // prove 500+-crowd geometry throughput (brief M6). Crowd draws are extra DrawInfo entries that
    // reference the SAME meshlet buffers (only the transform differs); active_draw_count_ switches
    // between the base set and base+crowd. Grid side N gives (N*N - 1) extra copies of the model.
    static constexpr uint32_t kCrowdGrid = 6;   // 6x6 = 35 extra copies (+ base) of the model
    bool crowd_enabled_ = false;
    // base_draw_count_/active_draw_count_ moved to GeometryScene (brief 11 P2).
    void build_crowd(const glm::vec3& aabb_min, const glm::vec3& aabb_max);
    void build_meshlet_gpu(String::engine_context& context);
    // Brief 04d: `phase` (0 legacy/transparency, 1 = bit-set only, 2 = bit-clear+HiZ+update) is pushed
    // into MeshletPush so the task shader partitions the worklist's meshlets across the two phases.
    void record_meshlet_draws(string::gpu::command_recorder& recorder, const string::gpu::pipeline& p,
                              uint16_t current_frame, const Worklist& wl, uint32_t phase = 0);

    // --- Brief 04d two-phase occlusion state ---------------------------------------------------
    // Persistent per-meshlet visibility bitfield (1 bit per GLOBAL meshlet id, ceil(total/32) words),
    // device-local, zero-initialized on creation. NOT ring-buffered — the temporal visibility state
    // must accumulate across frames. Phase 1 reads it; phase 2 sets/clears it (atomics). A cleared bit
    // => the meshlet takes phase 2 for one frame (warmup = fallback = streaming/LOD-change path).
    string::gpu::resource_id visbits_buffer_ = 0;
    uint32_t visbits_words_ = 0;
    // Per-draw LOD selected LAST frame (device-local, one word per draw). The draw-cull compute
    // compares it to this frame's selected LOD; a switch clears that draw's bitfield range (its
    // meshlets become "new" -> phase 2), because bits are keyed to the CURRENT LOD's meshlet ids.
    string::gpu::resource_id prev_draw_lod_buffer_ = 0;
    // True when the two-phase path is active this frame (HiZ enabled + warmup done). Gates
    // breaks_scene_group(): when false the pass renders single-pass (phase 0) in one group.
    bool two_phase_active_ = false;
    // Records phase-1 or phase-2 opaque+two-sided draws into the (already-open) MSAA render pass.
    void record_opaque_phase(string::gpu::command_recorder& recorder, uint16_t current_frame, uint32_t phase);
    // Zeroes visbits_buffer_ + prev_draw_lod_buffer_ (teleport/first-frame/streaming full clear).
    bool visbits_clear_pending_ = true;

    // overlay_stats_ moved to GeometryScene (brief 11 P2).

public:
    // Latest GPU culling stats (read back one frame late) for the UI overlay.
    const GpuMeshStats& mesh_stats() const { return stats_latest_; }
    // model_paths are .gltf/.glb files resolved relative to context.resources_path, flattened and
    // merged into one draw set (shared vertex/index/material/texture tables — the NewSponza packs
    // overlay the same world space). `overlay_stats` (may be null) receives the meshlet culling
    // stats + path/HiZ/view flags each frame for the UI overlay.
    // `lookdev` (brief 07): ignore model_paths and build the standing material-probe scene
    // instead — a roughness x metallic sphere grid + a white/mirror pair over a neutral ground
    // slab, baked through the same cook library (one draw per sphere; factors drive the
    // materials, no textures). The permanent lookdev sandbox: STRING_SCENE=lookdev ./run.sh.
    GeometryPass(String::engine_context& context, std::vector<std::filesystem::path> model_paths,
                 std::shared_ptr<MeshOverlayStats> overlay_stats = nullptr, bool lookdev = false);
    virtual ~GeometryPass() override;

    // Stable identity for tooling (Tracy zones, inspector). Brief 06. The renderer names the
    // pass-level GPU/CPU zones from this; finer per-stage zones (draw-cull, HiZ, shadows, sky,
    // transparency) are scoped inside record_compute/record via STRING_PROFILE_GPU_ZONE.
    std::string_view debug_name() const override { return "geometry"; }

    virtual void update(float delta_time, uint16_t current_frame) override;
    virtual bool record_compute(string::gpu::command_recorder& recorder, uint16_t current_frame) override;
    virtual void record(string::gpu::command_recorder& recorder, uint16_t current_frame) override;

    // Froxel light binning (the async-compute chain) is its own FroxelPass now (brief 11 step 3).

    // Brief 04d/11-step-2 two-phase occlusion. record() draws PHASE 1 (sky + last-frame-visible opaque
    // into the MSAA targets; the group MIN-resolves depth into hz.depth via the DepthResolve usage).
    // record_hiz() builds the HiZ pyramid (run by the standalone hiz.build compute pass). record_phase2()
    // draws PHASE 2 (the disocclusion complement + transparency) into the reloaded MSAA group (run by the
    // geometry.phase2 pass). When HiZ is off / warming up, two_phase_active_ is false: record() renders
    // everything single-pass and record_hiz()/record_phase2() no-op. These are ordinary public methods
    // now (the HizBuildPass / GeometryPhase2Pass sub-passes call them) — no base-Pass hooks.
    void record_hiz(string::gpu::command_recorder& recorder, uint16_t current_frame);
    void record_phase2(string::gpu::command_recorder& recorder, uint16_t current_frame);
    // Brief 11 M2: the cascaded shadow-map depth render. Extracted from record_compute into its own
    // public method so the standalone ShadowPass (a scheduling seam, like GtaoPass) can schedule it —
    // GeometryPass keeps the state (shadow images/slots, per-cascade worklists, the meshlet-shadow
    // program), the pass just calls this in the compute prepass, AFTER this pass's draw-cull/expand
    // produced the per-cascade worklists and BEFORE the MSAA group's lit draws sample the maps. The
    // block is self-contained (it hand-transitions its own maps; record_expand already barriered the
    // worklist reads), so relocating it leaves the command stream byte-identical. No-ops when shadows
    // are unconfigured. Returns true if it recorded work (parity with record_gtao_if_enabled).
    bool record_shadows_if_enabled(string::gpu::command_recorder& recorder, uint16_t current_frame);
    // Brief 11 M2: probe-GI capture (run-once, load-time) + relight (amortized). Extracted from the
    // head of record_compute into its own public method so the standalone GiPass (a scheduling seam)
    // can schedule it in the compute prepass — ordered after IblPass (this frame's SH) and before
    // ShadowPass (the relight->shadow-write WAR edge). All barriers are internal; byte-identical.
    bool record_gi_if_enabled(string::gpu::command_recorder& recorder, uint16_t current_frame);
    // Brief 11 M2: the sorted transparency draw (CPU back-to-front list build + one indirect draw).
    // Extracted from record()/record_phase2 into its own public method so the standalone
    // TransparencyPass can draw it as the LAST pass of the reopened MSAA group (depth-tested vs the
    // final opaque depth, no depth write, alpha-blended) — the same in-group position it held inside
    // record_phase2, so the output is byte-identical. GeometryPass keeps the state (per-frame list
    // buffers, the blend-draw indices, the transparent pipeline). No-ops when there's no scene.
    void record_transparency_pass(string::gpu::command_recorder& recorder, uint16_t current_frame);
    // Brief 11 step 3: GTAO is woven into the geometry depth pipeline (it reads the prev-slot resolved
    // hz.depth + the reprojection matrices record_hiz captures, and feeds SceneData). So GeometryPass
    // keeps its state; the standalone GtaoPass (prepass) just schedules this — running the half-res AO
    // chain iff the go/no-go decided in update() said so. Returns true if it recorded work.
    bool record_gtao_if_enabled(string::gpu::command_recorder& recorder, uint16_t current_frame)
    {
        if (!gtao_runs_this_frame_) return false;
        record_gtao(recorder, current_frame);
        return true;
    }
    bool two_phase_active() const { return two_phase_active_; }
    string::gpu::resource_id color_target() const { return color_target_; }
    string::gpu::resource_id depth_target() const { return depth_target_; }
    // Brief 11 step 3: hand out the shared scene state (the privately-inherited GeometryScene) so the
    // decomposed sub-passes (sky, ... ) reference it directly instead of reaching back into GeometryPass.
    // GeometryPass keeps inheriting it (zero body churn); the upcast is legal from within this member.
    GeometryScene* scene() { return this; }
    // Brief 11 step 2b: per-slot HiZ resources + the visibility bitfield, so the hiz.build / phase2
    // sub-passes declare real usages and the graph derives their barriers (resolve->read, pyramid
    // producer->consumer, phase1->phase2 bitfield RMW) instead of record_hiz hand-rolling them.
    string::gpu::resource_id hiz_depth_id(uint16_t frame) const
    {
        return frame < hiz_.size() ? hiz_[frame].depth : 0;
    }
    string::gpu::resource_id hiz_pyramid_id(uint16_t frame) const
    {
        return frame < hiz_.size() ? hiz_[frame].image : 0;
    }
    // Brief 16: the LOGICAL hiz depth + pyramid ring handles. The hiz.build/phase2 passes + the
    // DepthResolve marker declare these; the executor resolves the per-frame physical.
    string::gpu::image hiz_depth_ring() const { return hiz_depth_ring_; }
    string::gpu::image hiz_pyramid_ring() const { return hiz_pyramid_ring_; }
    string::gpu::resource_id visbits_id() const { return visbits_buffer_; }
};

// Brief 11 step 2: the two-phase occlusion path expressed as three ordinary registered passes sharing
// the GeometryPass that owns the meshlet/HiZ subsystem. GeometryPass IS geometry.phase1 (its record()
// draws phase 1); these two contribute the other scheduled points. They declare real usages so the
// graph orders them (phase1 -> hiz.build -> phase2) and derives the group break — no special hooks.

// sky: the fullscreen procedural background, drawn (color only, no depth) into the scene MSAA group
// before the opaque geometry. Brief 11 step 3: a REAL registered pass now — it owns its SkyComponent
// and reads the shared sun/sky/camera state through GeometryScene, rather than being drawn inside
// GeometryPass::record(). Registered before geometry.phase1 so it's the first draw in the group.
class SkyPass final : public String::Pass
{
    string::gpu::device& device_;
    GeometryScene* scene_;
    SkyComponent sky_;
public:
    SkyPass(String::engine_context& context, GeometryScene* scene)
        : device_(context.device), scene_(scene)
    {
        sky_.init(context);
        usages = { { context.color_target, String::Access::ColorWrite,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT } };
        // Brief 11 M3: r.pass.sky drops the sky pass -> the MSAA group's clear shows behind the scene.
        enable_predicate = [] { return cv_pass_sky().get(); };
    }
    ~SkyPass() override
    {
        if (string::gpu::shader_program* p = sky_.program())
        {
            const string::gpu::pipeline& pl = p->current();
            vkDestroyPipeline(device_.get_device(), pl.pipeline, nullptr);
            vkDestroyPipelineLayout(device_.get_device(), pl.pipeline_layout, nullptr);
        }
    }
    std::string_view debug_name() const override { return "sky"; }
    void record(string::gpu::command_recorder& recorder, uint16_t) override
    {
        GeometryScene& s = *scene_;
        if (s.draw_count_ == 0) return;   // matches GeometryPass::record's early-out (no scene, no sky)
        sky_.record(recorder, SkyParams{
            .view_proj = s.camera_.view_proj(),
            .camera_pos = s.camera_.position(),
            .sun_dir = s.sun_dir_,
            .sky_zenith = s.sky_zenith_,
            .sky_ground = s.sky_ground_,
            .sun_color = s.sun_color_,
            .sun_intensity = s.sun_intensity_,
            .furnace = s.furnace_,
        });
    }
};

// froxel.cull: the Forward+ light-binning compute chain. Brief 11 step 3: a real registered pass that
// OWNS its FroxelComponent and publishes it into GeometryScene (scene.froxel) so GeometryPass's
// SceneData reads the grid dims + froxel buffer address. It is the frame's dependency-free async chain
// (has_async_compute + async_usages); the renderer places it on an async lane or inline. compute_only
// so it forms no render group — its record() is a no-op (all work is in record_async_compute).
class FroxelPass final : public String::Pass
{
    string::gpu::device& device_;
    GeometryScene* scene_;
    FroxelComponent froxel_;
public:
    FroxelPass(String::engine_context& context, GeometryScene* scene)
        : device_(context.device), scene_(scene)
    {
        froxel_.init(context, context.frames_in_flight);
        scene_->froxel = &froxel_;
    }
    ~FroxelPass() override
    {
        if (string::gpu::shader_program* p = froxel_.program())
        {
            const string::gpu::pipeline& pl = p->current();
            vkDestroyPipeline(device_.get_device(), pl.pipeline, nullptr);
            vkDestroyPipelineLayout(device_.get_device(), pl.pipeline_layout, nullptr);
        }
        froxel_.destroy();
    }
    std::string_view debug_name() const override { return "froxel.cull"; }
    // Nature (compute-only + async) is declared fluently by the app when authoring the graph.
    bool async_has_work() const { return froxel_.has_work(); }   // dynamic gate for PassSpec.async()
    void record(string::gpu::command_recorder&, uint16_t) override {}   // async-only; nothing inline
    // Brief 16 M2: (re)allocate the registry-owned froxel ring at the DETERMINISTIC resize point
    // (init + window resize, before any per-frame update) — not lazily in update() where it raced
    // GeometryPass's SceneData read. The froxel address is now valid from frame 0 for any pass order.
    void resize(VkExtent2D extent) override
    {
        String::Pass::resize(extent);
        froxel_.ensure_capacity(screen_size);
    }
    void update(float, uint16_t current_frame) override
    {
        async_usages.clear();
        if (froxel_.handle().valid())
        {
            // Brief 16: declare the LOGICAL froxel handle; the executor resolves the per-frame physical.
            async_usages.push_back({ .access = String::Access::StorageWrite,
                                     .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .buf = froxel_.handle() });
            async_usages.push_back({ .access = String::Access::StorageRead,
                                     .stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, .buf = froxel_.handle() });
        }
    }
    void record_async_compute(string::gpu::command_recorder& recorder, uint16_t current_frame) override
    {
        if (!froxel_.active(current_frame)) return;
        GeometryScene& s = *scene_;
        const uint32_t light_count = s.lights_enabled_ ? static_cast<uint32_t>(s.lights_.size()) : 0u;
        const glm::mat4 proj = s.camera_.view_proj() * glm::inverse(s.camera_.view());  // == projection
        froxel_.record(recorder, current_frame, FroxelParams{
            .view = s.camera_.view(),
            .inv_proj = glm::inverse(proj),
            .screen = glm::uvec2(screen_size.width, screen_size.height),
            .near_plane = s.camera_.near_plane(),
            .far_plane = std::min(s.settings_.shadow_depth_range, s.camera_.far_plane()),
            .light_count = light_count,
            .lights = light_count > 0
                    ? s.resources->address(s.light_buffer_, current_frame) : 0,
        });
    }
};

// ibl.update: the dynamic sky image-based-lighting compute chain (env capture -> mips -> SH +
// prefilter + DFG). Brief 11 step 3: a real prepass-compute pass that OWNS its IblComponent and
// publishes it into GeometryScene (scene.ibl) for SceneData + probe GI to read. Runs BEFORE the
// geometry pass so this frame's draws + GI relight read this frame's environment. Amortized (only
// records when the sun moved past the trigger); the whole chain updates in one frame.
class IblPass final : public String::Pass
{
    GeometryScene* scene_;
    IblComponent ibl_;
    uint64_t frames_ = 0;   // local counter for the debug numeric-verification gate (STRING_IBL_VERIFY)
public:
    IblPass(String::engine_context& context, GeometryScene* scene) : scene_(scene)
    {
        ibl_.init(context);
        scene_->ibl = &ibl_;
    }
    ~IblPass() override { ibl_.destroy(); }
    std::string_view debug_name() const override { return "ibl.update"; }
    void record(string::gpu::command_recorder&, uint16_t) override {}   // prepass-only; no group work
    void update(float, uint16_t) override
    {
        GeometryScene& s = *scene_;
        // Trigger the amortized update (sun moved past the delta, furnace flip, first frame, or forced).
        ibl_.begin_frame(s.sun_dir_, s.furnace_, cv_ibl_every_frame().get());
        if (cv_ibl_verify().get() && ++frames_ == 40)
            ibl_.run_verification(s.furnace_);
        // Producer side of the SH buffer: declare the write only on frames the chain actually runs
        // (the lit-fragment + GI read side is declared by GeometryPass). Empty otherwise.
        usages.clear();
        if (ibl_.sh_buffer() != 0 && ibl_.update_pending())
            usages.push_back({ ibl_.sh_buffer(), String::Access::StorageWrite,
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT });
    }
    bool record_compute(string::gpu::command_recorder& recorder, uint16_t) override
    {
        if (!ibl_.needs_update()) return false;
        GeometryScene& s = *scene_;
        ibl_.record_update(recorder, IblLighting{
            .sun_dir = s.sun_dir_, .sky_zenith = s.sky_zenith_, .sky_ground = s.sky_ground_,
            .sun_color = s.sun_color_, .sun_intensity = s.sun_intensity_, .furnace = s.furnace_ });
        return true;
    }
};

// gtao: the half-res GTAO + bent-normal chain (prev-frame depth -> this slot's AO). Brief 11 step 3:
// a registered prepass pass, but a SCHEDULING SEAM over GeometryPass (which keeps the state) — gtao is
// woven into the geometry depth pipeline (prev-slot hz.depth + record_hiz's reprojection matrices +
// SceneData), so it is part of that cohesive subsystem, not an independent service like sky/froxel/ibl.
// Runs in the prepass, before geometry, so this frame's lit fragments read a finished AO texture.
class GtaoPass final : public String::Pass
{
    GeometryPass* geo_;
public:
    explicit GtaoPass(GeometryPass* geo) : geo_(geo)
    {
        // Brief 11 M3: r.gtao.enabled drops the AO pass AND (same cvar) makes SceneData.gtao_slot
        // invalid (0xFFFFFFFF) so the lit shader skips AO — the graceful degrade is already data-level.
        enable_predicate = [] { return cv_gtao_enabled().get(); };
    }
    std::string_view debug_name() const override { return "gtao"; }
    void record(string::gpu::command_recorder&, uint16_t) override {}   // prepass-only; no group work
    bool record_compute(string::gpu::command_recorder& recorder, uint16_t current_frame) override
    {
        return geo_->record_gtao_if_enabled(recorder, current_frame);
    }
};

// gi.probe: relightable irradiance-probe capture (run-once) + relight (amortized). Brief 11 M2: a
// registered pass, scheduling seam over GeometryPass (which keeps the probe volume + atlases + programs)
// — probe GI is woven into the meshlet/lighting subsystem (relight reads this frame's sky SH + the CSM
// shadow maps; shading samples the irradiance atlas), so it is a cohesive sub-stage, not an independent
// service. Runs in the compute prepass, ordered after ibl.update (this frame's SH) and before
// shadow.cascades (relight's shadow-map reads must precede this frame's shadow depth writes — the WAR
// execution edge). Atlases stay local-class hand-managed this brief; it declares no framework usages.
class GiPass final : public String::Pass
{
    GeometryPass* geo_;
public:
    explicit GiPass(GeometryPass* geo) : geo_(geo)
    {
        // Brief 11 M3: r.gi drops the probe-GI pass AND (via the same cvar) makes SceneData.probe_gi 0,
        // so shading falls back to the sky-SH ambient — the graceful degrade is already data-level.
        enable_predicate = [] { return cv_gi_enabled().get(); };
    }
    std::string_view debug_name() const override { return "gi.probe"; }
    void record(string::gpu::command_recorder&, uint16_t) override {}   // prepass-only; no group work
    bool record_compute(string::gpu::command_recorder& recorder, uint16_t current_frame) override
    {
        return geo_->record_gi_if_enabled(recorder, current_frame);
    }
};

// shadow.cascades: the cascaded shadow-map depth render (one depth-only meshlet draw per cascade into
// per-frame D32 maps). Brief 11 M2: a registered pass, but a SCHEDULING SEAM over GeometryPass (which
// keeps the shadow images/worklists/program) — shadows are woven into the meshlet subsystem (the
// per-cascade worklists come from GeometryPass's draw-cull/expand, the lit fragments sample the maps),
// so this is a cohesive meshlet sub-stage, not an independent service like sky/froxel/ibl. Runs in the
// compute prepass, AFTER geometry.phase1's record_compute produced the worklists and BEFORE the MSAA
// group's lit draws — hence authored after geometry in the plan so its record_compute is ordered later.
// Shadow maps stay local-class hand-managed this brief (documented brief-11 boundary), so it declares no
// framework usages: the block hand-transitions its own maps and record_expand barriered the reads.
class ShadowPass final : public String::Pass
{
    GeometryPass* geo_;
public:
    explicit ShadowPass(GeometryPass* geo) : geo_(geo)
    {
        // Brief 11 M3: r.pass.shadow drops the shadow render AND GeometryPass::update() sets SceneData
        // cascade_count 0 off the same cvar -> the lit shader's shadow term cancels (unshadowed scene).
        enable_predicate = [] { return cv_pass_shadow().get(); };
    }
    std::string_view debug_name() const override { return "shadow.cascades"; }
    void record(string::gpu::command_recorder&, uint16_t) override {}   // prepass-only; no group work
    bool record_compute(string::gpu::command_recorder& recorder, uint16_t current_frame) override
    {
        return geo_->record_shadows_if_enabled(recorder, current_frame);
    }
};

// hiz.build: the compute step between the two MSAA groups (MIN-resolved depth -> HiZ pyramid).
class HizBuildPass final : public String::Pass
{
    GeometryPass* geo_;
public:
    explicit HizBuildPass(GeometryPass* geo) : geo_(geo) {}
    std::string_view debug_name() const override { return "hiz.build"; }
    // Brief 11 step 2b: declare the per-slot HiZ I/O so the renderer derives the barriers — reads the
    // MIN-resolved depth (seeded post-resolve), writes the whole pyramid. Refreshed each frame (the
    // resources rotate per frame-in-flight slot). Empty until the pyramid exists (early frames).
    void update(float, uint16_t current_frame) override
    {
        usages.clear();
        // Brief 16: declare the LOGICAL hiz depth + pyramid handles; the executor resolves per frame.
        if (geo_->hiz_depth_ring().valid())
            usages.push_back({ .access = String::Access::SampledRead,
                               .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .img = geo_->hiz_depth_ring() });
        if (geo_->hiz_pyramid_ring().valid())
            usages.push_back({ .access = String::Access::StorageImageWrite,
                               .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .img = geo_->hiz_pyramid_ring() });
    }
    void record(string::gpu::command_recorder& recorder, uint16_t current_frame) override
    {
        geo_->record_hiz(recorder, current_frame);
    }
};

// geometry.phase2: the disocclusion-complement opaque + probe-debug + transparency draws into the
// reloaded MSAA group. Declares Color/Depth so it forms that group (loads phase-1's MSAA, resolves
// color at the chain's end); the record no-ops when the two-phase path is inactive.
class GeometryPhase2Pass final : public String::Pass
{
    GeometryPass* geo_;
public:
    explicit GeometryPhase2Pass(GeometryPass* geo) : geo_(geo) { refresh(0); }
    std::string_view debug_name() const override { return "geometry.phase2"; }
    // Brief 11 step 2b: Color+Depth form the reloaded MSAA group; the per-slot pyramid SampledRead (task
    // stage) derives the hiz.build->phase2 handoff; the visbits StorageWrite (task stage) derives the
    // phase1-read -> phase2-RMW bitfield barrier. Refreshed each frame (pyramid rotates per slot).
    void update(float, uint16_t current_frame) override { refresh(current_frame); }
    void record(string::gpu::command_recorder& recorder, uint16_t current_frame) override
    {
        geo_->record_phase2(recorder, current_frame);
    }
private:
    void refresh(uint16_t frame)
    {
        usages.clear();
        usages.push_back({ geo_->color_target(), String::Access::ColorWrite,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT });
        usages.push_back({ geo_->depth_target(), String::Access::DepthWrite,
                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT });
        // Brief 16: pyramid via LOGICAL handle (executor resolves per frame); visbits is a persistent
        // (non-ring) buffer, so it stays a raw id.
        if (geo_->hiz_pyramid_ring().valid())
            usages.push_back({ .access = String::Access::SampledRead,
                               .stage = VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT, .img = geo_->hiz_pyramid_ring() });
        if (auto v = geo_->visbits_id())
            usages.push_back({ v, String::Access::StorageWrite, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT });
    }
};

// transparency: the sorted alpha-blended draw. Brief 11 M2: a registered pass, scheduling seam over
// GeometryPass (which keeps the per-frame list buffers + transparent pipeline). Declares ColorWrite +
// DepthRead so it joins the reopened MSAA group (same color target as geometry.phase2) as that group's
// LAST pass — the exact in-group position it held inside record_phase2 (depth-tested vs the final opaque
// depth, no depth write, blended). Authored right after phase2 so it's the last draw of that group.
class TransparencyPass final : public String::Pass
{
    GeometryPass* geo_;
public:
    explicit TransparencyPass(GeometryPass* geo) : geo_(geo)
    {
        refresh();
        // Brief 11 M3: r.pass.transparency drops the blended draw -> opaque geometry only (no degrade
        // needed; transparency is a pure over-draw with no consumer).
        enable_predicate = [] { return cv_pass_transparency().get(); };
    }
    std::string_view debug_name() const override { return "transparency"; }
    void update(float, uint16_t) override { refresh(); }
    void record(string::gpu::command_recorder& recorder, uint16_t current_frame) override
    {
        geo_->record_transparency_pass(recorder, current_frame);
    }
private:
    void refresh()
    {
        // ColorWrite = same COLOR_TARGET as phase2 -> the renderer groups this pass into the reopened
        // MSAA group. DepthRead (test only) — the group already forces depth to write scope for its
        // attachment, so this just declares the honest dependency (transparency reads the opaque depth).
        usages.clear();
        usages.push_back({ geo_->color_target(), String::Access::ColorWrite,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT });
        usages.push_back({ geo_->depth_target(), String::Access::DepthRead,
                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT });
    }
};

}  // namespace sandbox
