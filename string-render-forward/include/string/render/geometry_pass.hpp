#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/device.hpp>
#include <string/vulkan/render_data.hpp>
#include <string/vulkan/frame_graph.hpp>
#include <string/gpu/pass_context.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/scene/camera.hpp>
#include <string/gpu/pipeline.hpp>
#include "string/gpu/descriptor_allocator.hpp"
#include "string/gpu/residency_manager.hpp"
#include "string/gpu/resource.hpp"
#include "string/gpu/resource_allocator.hpp"

#include <string/render/gltf_types.hpp>
#include <string/render/geometry_streamer.hpp>
#include <string/render/texture_streamer.hpp>
#include <string/render/lighting_data.hpp>
#include <string/render/meshlet_data.hpp>
#include <string/render/probe_gi.hpp>
#include <string/render/meshlet_builder.hpp>
#include <string/render/scene_loader.hpp>
#include <string/render/render_cvars.hpp>
#include <string/render/geometry/geometry_scene.hpp>
#include <string/render/shadow_maps.hpp>
#include <string/render/sorted_transparency.hpp>
#include <string/render/gtao.hpp>
#include <string/render/probe_gi_component.hpp>
#include <string/render/geometry/sky_component.hpp>
#include <string/render/geometry/froxel_component.hpp>
#include <string/render/geometry/ibl_component.hpp>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <volk.h>

namespace string::render
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
// GtaoPush moved to lighting_data.hpp (brief 20): gtao.hpp needs it, and geometry_pass.hpp includes
// gtao.hpp, so keeping it here made a cycle.

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
// Brief 20: no base class. This is a plain object the application owns; its callbacks capture it.
// It still privately inherits GeometryScene, which is shared technique state, not a graph concept.
class geometry_pass final : private GeometryScene
{
    ::string::gpu::device& device_;
    ::string::gpu::resource_allocator& allocator_;
    ::string::gpu::descriptor_table& descriptor_table_;
    string::InputMap& input_map_;
    // Froxel light binning: extracted to FroxelComponent froxel_ (declared below, brief 11).

    // Brief 20: the colour/depth sentinels are gone. The scene attachments are viewport-scaled
    // transients the application declares and hands to declare() as logical handles.
    // draw_count_ / frames_in_flight_ moved to GeometryScene (brief 11 P2).

    // Geometry residency streaming: per-draw vertex/index ranges suballocate into heaps SMALLER than
    // the whole model as draws enter the view frustum, and are freed (reclaimed) on eviction. A
    // not-yet-resident draw stays hidden (DrawInfo.resident 0). Capping resident geometry below the full
    // model is what exercises reclaim; raise toward 100 for a VRAM-fitting scene with no pop-in.
    static constexpr std::uint64_t kGeometryResidentPercent = 100;
    static constexpr VkDeviceSize kGeometryStreamPerFrame = 32ull * 1024 * 1024;
    // Per-model budget (heap capacity), so this is a unique_ptr built once sizes are known.
    std::unique_ptr<::string::gpu::residency_manager> geometry_residency_;
    std::unique_ptr<GeometryStreamer> geometry_streamer_;
    // Only stream per-frame when the model doesn't fit the budget (< 100%). When it fits, all
    // geometry is uploaded up front (no per-frame streaming/eviction cost) — streaming a scene that
    // fits in VRAM just adds startup lag for no benefit.
    bool geometry_streaming_ = false;

    // Per glTF-image backing resource + its bindless slot (index-aligned with the loaded
    // model's textures, so a material's texture index maps straight to a slot).
    std::vector<::string::gpu::resource_id> texture_images_;
    std::vector<uint32_t> texture_slots_;
    // 1x1 white fallback, used for draws whose material has no base-color texture (the
    // base-color factor still tints it) — also the metallic-roughness fallback (white .g/.b = 1,
    // so metallic/roughness reduce to the scalar factors).
    ::string::gpu::resource_id white_image_;
    uint32_t white_slot_ = 0;
    // 1x1 flat-normal fallback (tangent-space +Z = RGBA 128,128,255), for draws with no normal map:
    // sampling it yields the geometric normal, so the shader needs no branch.
    ::string::gpu::resource_id flat_normal_image_;
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

    // Probe GI (brief 09b relightable irradiance volume): owned by GiPass (ProbeGi component),
    // reached via GeometryScene::gi.
    // Shadow maps: owned by ShadowPass (ShadowMaps component), reached via GeometryScene::shadow.
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
    ::string::gpu::residency_manager residency_;
    std::unique_ptr<TextureStreamer> texture_streamer_;
    std::vector<::string::gpu::resource_id> streamed_textures_;
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

    // cull_enabled_ moved to GeometryScene (shared by every meshlet draw path).

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
    ::string::gpu::shader_program* meshlet_program_ = nullptr;
    ::string::gpu::shader_program* meshlet_twosided_program_ = nullptr;  // brief 04: CULL_NONE variant
    ::string::gpu::shader_program* hiz_program_ = nullptr;
    ::string::gpu::shader_program* reset_program_ = nullptr;
    // Brief 04c: two-phase GPU cull for the global meshlet worklists. (1) draw-cull compute writes each
    // draw's surviving meshlet COUNT + selected LOD at a stable slot; (2) expand compute prefix-scans
    // the counts (order-stable, no atomics) into a compacted {draw_index, meshlet_local} worklist +
    // the indirect groupCount for a SINGLE vkCmdDrawMeshTasksIndirectEXT per list. Draw count no
    // longer drives dispatch cost.
    ::string::gpu::shader_program* draw_cull_program_ = nullptr;
    ::string::gpu::shader_program* expand_scan_blocks_program_ = nullptr;
    ::string::gpu::shader_program* expand_scan_carry_program_ = nullptr;
    ::string::gpu::shader_program* expand_fill_program_ = nullptr;
    // A worklist buffer packs, at 16B-aligned regions: counts[max_draws], offsets[max_draws],
    // block_sums[max_blocks] (scan scratch), then the COMPACTED draw list — commands[max_draws]
    // (12B VkDrawMeshTasksIndirectCommandEXT), records[max_draws] (8B {draw_index, lod}), and the
    // surviving-draw count word (for vkCmdDrawMeshTasksIndirectCountEXT). One command per surviving
    // draw (empty slots compacted out): removes the fan-out cost while keeping command ordering.
    // Brief 21 D4: one graph TRANSIENT per list, declared by the app and latched here. Single-backed,
    // as every transient is — the cross-frame edge against the previous frame's still-reading draws
    // is derived from the declarations, which is exactly the memory the per-slot arena was spending.
    WorklistSet wl_{};
    uint32_t cull_max_blocks_ = 0;      // ceil(max_draws / kScanBlock)
    // Draw phase for one camera frame (writes opaque+twosided counts + draw_lod), or a shadow cascade
    // (cascade>=0: resident-only counts vs the cascade sphere).
    void record_draw_cull(::string::pass_context& ctx, ::string::gpu::buffer stats, int cascade = -1);
    // Compaction phase for one worklist (scan + fill): compacts surviving draws into commands[]+records[]
    // + count, ready for one vkCmdDrawMeshTasksIndirectCountEXT. Brief 20: the scan is THREE declared
    // passes, one per dispatch, so the graph derives the two barriers that used to sit between them.
    void record_expand_scan_blocks(::string::pass_context& ctx, ::string::gpu::buffer list);
    void record_expand_scan_carry (::string::pass_context& ctx, ::string::gpu::buffer list);
    void record_expand_fill       (::string::pass_context& ctx, ::string::gpu::buffer list);

    // Brief 04 sorted transparency pass. BLEND draws are excluded from the opaque lists (GPU compute)
    // and rendered here after opaque + sky: depth-tested vs opaque depth, NO depth write, alpha-blended,
    // one CPU back-to-front-sorted COMPACTED per-draw command list (commands[] then records[] then
    // count in a single host-visible buffer per frame — same shape as the GPU-compacted lists, so the
    // same task/mesh consumer + vkCmdDrawMeshTasksIndirectCountEXT draws it). Draw-order (back-to-front)
    // is preserved because the CPU writes commands/records in sorted order. Counts are small (no OIT).
    // Transparency list buffers + blend draw indices owned by TransparencyPass (SortedTransparency).

    // HiZ depth pyramid (R32F mip chain), one per frame in flight. Built each frame from the scene
    // depth via a MIN reduce (reverse-Z: min = farthest). Sampled by the pass-2 task shader.
    // Brief 20: the HiZ pyramid is a GRAPH resource. What survives here is its SHAPE — the only part
    // the technique still needs, to size its dispatches. The images, the per-mip views, the bindless
    // slots and the hand-rolled sampler are all gone: the pyramid is declared by the application and
    // every slot comes from ctx.slot(pyramid.mip(m)) at record time.
    struct HizPyramid
    {
        uint32_t mips = 0;
        glm::uvec2 size{ 0, 0 };
    };
    std::vector<HizPyramid> hiz_;
    uint32_t hiz_screen_w_ = 0, hiz_screen_h_ = 0;
    void ensure_hiz(uint16_t current_frame);

    // --- Brief 09: half-res GTAO with bent normals ----------------------------------------------
    // Computed at the top of record_compute from the PREVIOUS frame slot's min-resolved depth
    // (hz.depth) under that frame's matrices; consumed by this frame's lit fragments through
    // SceneData.prev_view_proj reprojection (exact for the static scene; 1-frame-late AO, zero
    // temporal accumulation so nothing can ghost). raw -> denoise -> per-slot final (RGBA8:
    // rgb bent world normal, a visibility). Requires the two-phase HiZ depth; auto-off otherwise.
    // GTAO targets/slots/sampler/programs owned by GtaoPass (GtaoChain component).

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
    // dbg.orbit motion lever: continuously sways the camera around a captured base pose so headless
    // captures exercise per-frame disocclusion (the two-phase interleaved phase-1/phase-2 path).
    // Base pose is latched on the first orbiting frame; the sway is applied AFTER camera_.update.
    bool orbit_base_latched_ = false;
    glm::vec3 orbit_base_pos_{ 0.0f };
    float orbit_base_yaw_ = 0.0f;
    float orbit_base_pitch_ = 0.0f;
    float orbit_phase_ = 0.0f;       // accumulated angle (rad)
    // Crowd stress scene (K): the base draws duplicated across a grid with varied transforms, to
    // prove 500+-crowd geometry throughput (brief M6). Crowd draws are extra DrawInfo entries that
    // reference the SAME meshlet buffers (only the transform differs); active_draw_count_ switches
    // between the base set and base+crowd. Grid side N gives (N*N - 1) extra copies of the model.
    static constexpr uint32_t kCrowdGrid = 6;   // 6x6 = 35 extra copies (+ base) of the model
    bool crowd_enabled_ = false;
    // base_draw_count_/active_draw_count_ moved to GeometryScene (brief 11 P2).
    void build_crowd(const glm::vec3& aabb_min, const glm::vec3& aabb_max);
    void build_meshlet_gpu(string::engine_context& context);
    // Brief 04d: `phase` (0 legacy/transparency, 1 = bit-set only, 2 = bit-clear+HiZ+update) is pushed
    // into MeshletPush so the task shader partitions the worklist's meshlets across the two phases.
    void record_meshlet_draws(::string::pass_context& ctx, const ::string::gpu::pipeline& p,
                              ::string::gpu::buffer list, uint32_t phase,
                              ::string::gpu::image pyramid,
                              ::string::gpu::buffer scene_data, ::string::gpu::buffer stats);

    // --- Brief 04d two-phase occlusion state ---------------------------------------------------
    // Persistent per-meshlet visibility bitfield (1 bit per GLOBAL meshlet id, ceil(total/32) words),
    // device-local, zero-initialized on creation. NOT ring-buffered — the temporal visibility state
    // must accumulate across frames. Phase 1 reads it; phase 2 sets/clears it (atomics). A cleared bit
    // => the meshlet takes phase 2 for one frame (warmup = fallback = streaming/LOD-change path).
    // visbits_buffer_ moved to GeometryScene (named by every meshlet push constant).
    uint32_t visbits_words_ = 0;
    // Per-draw LOD selected LAST frame (device-local, one word per draw). The draw-cull compute
    // compares it to this frame's selected LOD; a switch clears that draw's bitfield range (its
    // meshlets become "new" -> phase 2), because bits are keyed to the CURRENT LOD's meshlet ids.
    ::string::gpu::resource_id prev_draw_lod_buffer_ = 0;
    // True when the two-phase path is active this frame (HiZ enabled + warmup done). Gates
    // breaks_scene_group(): when false the pass renders single-pass (phase 0) in one group.
    bool two_phase_active_ = false;
    // Records phase-1 or phase-2 opaque+two-sided draws into the (already-open) MSAA render pass.
    void record_opaque_phase(::string::pass_context& ctx, uint32_t phase,
                             ::string::gpu::image pyramid,
                             ::string::gpu::buffer scene_data, ::string::gpu::buffer stats);
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
    geometry_pass(string::engine_context& context, VkSampleCountFlagBits samples,
                  std::vector<std::filesystem::path> model_paths,
                  std::shared_ptr<MeshOverlayStats> overlay_stats = nullptr, bool lookdev = false);
    ~geometry_pass();

    // Per-frame CPU work — camera, time-of-day, streaming feedback, SceneData fill, stats readback.
    // Ordinary app code called before the graph executes; it was never a graph concept, it only
    // looked like one because it arrived as a Pass virtual.
    void tick(float delta_time, uint32_t frame_slot);

    // Author every pass this subsystem owns onto the graph: phase 1, the HiZ mip chain, phase 2, the
    // meshlet cull/expand chain and the draws. The attachments and worklists arrive as logical
    // handles the application declared.
    void declare(string::frame_graph& fg, string::gpu::image color, string::gpu::image resolve,
                 string::gpu::image depth,
                 string::gpu::image hiz_depth, string::gpu::image hiz_pyramid,
                 const WorklistSet& worklists, string::gpu::buffer scene_data,
                 string::gpu::buffer lights, string::gpu::buffer stats,
                 // Everything SceneData points at. Declaring them on scene.upload is what makes the
                 // slots it writes resolvable at record time — and what derives the edges from each
                 // producer to the draws that read SceneData.
                 std::span<const string::gpu::image> cascades, string::gpu::image gtao_ao,
                 string::gpu::image env_prefiltered, string::gpu::image dfg_lut,
                 string::gpu::buffer ibl_sh, string::gpu::buffer froxels);

    // How many entries the probe-GI capture table needs for this scene's meshlets. The app sizes the
    // table buffer from it, so the declaration and the fill agree by construction.
    std::size_t gi_capture_entries() const;

    // The HiZ pyramid's shape for a given viewport. The graph derives the IMAGE from the same
    // relationship (viewport_fit::half_pow2 + all_mips); this is what the dispatch shape and the
    // per-mip conditionals are computed from, so both halves stay in agreement by construction
    // rather than by the app passing a number twice.
    static VkExtent2D hiz_extent(VkExtent2D viewport);
    static uint32_t hiz_mip_count(VkExtent2D viewport);
    // Levels the reduction chain is AUTHORED at. Fixed, and larger than any real viewport needs
    // (2^15 px); the levels a given extent does not reach are conditioned off rather than
    // re-authored. This is what "authored once" costs for a variable-length chain: a handful of
    // declarations that never survive.
    static constexpr uint32_t kMaxHizMips = 16;

private:
    // One declared pass per HiZ mip: mip 0 reduces the resolved scene depth, each later mip reduces
    // the one above it. The graph derives the chain from the slice declarations.
    // Graph handles this subsystem was declared against, latched in declare() so the record bodies
    // can name them. They are LOGICAL — every resolution still happens through pass_context.
    // MSAA sample count of the scene attachments. The APP declares those, so the app states this;
    // the renderer no longer has an opinion to supply.
    // Per-slot: has this frame slot's worklist arena been zeroed yet? See record_cull.
    // One-shot zero of every declared work list (see record_cull). Transients are single-backed, so
    // this is one flag, not one per frame slot.
    bool lists_zeroed_ = false;
    VkSampleCountFlagBits scene_samples_ = VK_SAMPLE_COUNT_1_BIT;
    string::gpu::buffer scene_data_{}, lights_buffer_{}, stats_{}, froxels_{}, ibl_sh_{};
    // The persistent visibility bitfield, as a GRAPH handle over the buffer this pass owns. It is
    // not a graph-allocated resource — use_persistent lets the graph manage its USAGE only, which is
    // the whole point: the cross-frame task-stage RMW edge has to be derived from a declaration.
    string::gpu::buffer visbits_{};
    string::gpu::image  pyramid_{}, gtao_ao_{}, env_prefiltered_{}, dfg_lut_{};
    std::array<string::gpu::image, kMaxCascades> cascades_{};

    // The GPU draw-cull + list-expansion chain (defined in geometry_pass_meshlet.cpp).
    void declare_cull(string::frame_graph& fg, string::gpu::buffer stats);
    void declare_expand(string::frame_graph& fg);
    // The list a cull/expand pass index names. kListOpaque / kListTwosided / kListCascade0 + c.
    string::gpu::buffer list_of(uint32_t list) const;
    void declare_hiz(string::frame_graph& fg, string::gpu::image depth, string::gpu::image pyramid);
    void record_hiz_mip(string::pass_context& ctx, uint32_t m, string::gpu::image depth,
                        string::gpu::image pyramid);

public:

    // Brief 04d/11-step-2 two-phase occlusion. record() draws PHASE 1 (sky + last-frame-visible opaque
    // into the MSAA targets; the group MIN-resolves depth into hz.depth via the DepthResolve usage).
    // record_hiz() builds the HiZ pyramid (run by the standalone hiz.build compute pass). record_phase2()
    // draws PHASE 2 (the disocclusion complement + transparency) into the reloaded MSAA group (run by the
    // geometry.phase2 pass). When HiZ is off / warming up, two_phase_active_ is false: record() renders
    // everything single-pass and record_hiz()/record_phase2() no-op. These are ordinary public methods
    // now (the HizBuildPass / GeometryPhase2Pass sub-passes call them) — no base-Pass hooks.
    void record_scene_upload(string::pass_context& ctx);
    void record_phase1(string::pass_context& ctx);
    // The three per-frame resets, one declared pass each (see declare()).
    void record_reset_lists(string::pass_context& ctx);
    void record_reset_stats(string::pass_context& ctx);
    void record_reset_visbits(string::pass_context& ctx);
    void record_phase2(string::pass_context& ctx);
    // Brief 11 M2: the cascaded shadow-map depth render. Extracted from record_compute into its own
    // public method so the standalone ShadowPass (a scheduling seam, like GtaoPass) can schedule it —
    // Brief 11 M2: probe-GI capture (run-once, load-time) + relight (amortized). Extracted from the
    // head of record_compute into its own public method so the standalone GiPass (a scheduling seam)
    // can schedule it in the compute prepass — ordered after IblPass (this frame's SH) and before
    // ShadowPass (the relight->shadow-write WAR edge). All barriers are internal; byte-identical.
    bool record_gi_if_enabled(::string::gpu::command_recorder& recorder, uint16_t current_frame);
    bool two_phase_active() const { return two_phase_active_; }
    // Brief 11 step 3: hand out the shared scene state (the privately-inherited GeometryScene) so the
    // decomposed sub-passes (sky, ... ) reference it directly instead of reaching back into GeometryPass.
    // GeometryPass keeps inheriting it (zero body churn); the upcast is legal from within this member.
    GeometryScene* scene() { return this; }
    // Brief 11 step 2b: per-slot HiZ resources + the visibility bitfield, so the hiz.build / phase2
    // sub-passes declare real usages and the graph derives their barriers (resolve->read, pyramid
    // producer->consumer, phase1->phase2 bitfield RMW) instead of record_hiz hand-rolling them.
    // Brief 20: the hiz id/ring accessors are gone. The pyramid and the resolved depth are graph
    // resources the application declares and hands to declare(); nothing outside needs their ids.
    ::string::gpu::resource_id visbits_id() const { return visbits_buffer_; }
    // The probe irradiance atlas: relit by gi.probe, sampled by every lit fragment (and by next
    // frame's bounce). The capture atlases (gbuf/albedo/vis) are written AND consumed inside gi.probe,
    // so they stay intra-pass. 0 until the volume is built (r.gi off => never).
    // Brief 20: the IBL SH buffer is an app-declared graph buffer; gi.relight declares .reads(sh)
    // and the graph derives the edge. No id accessor needed.
};

// Brief 20: the nine wrapper `Pass` subclasses that used to live here — SkyPass, FroxelPass, IblPass,
// GtaoPass, GiPass, ShadowPass, HizBuildPass, GeometryPhase2Pass, TransparencyPass — are DELETED.
//
// Every one of them existed only to give a component a place to hang two things the graph now owns:
// a hand-written `usages` vector refreshed each frame, and an `enable_predicate`. Several were pure
// scheduling seams with no state at all (HizBuildPass held a single `GeometryPass*` and forwarded
// one call). The components they wrapped — sky_component, froxel_component, ibl_component,
// gtao_chain, probe_gi_component, shadow_maps, sorted_transparency — are plain objects that declare
// themselves onto the graph through their own declare(), and the application owns them.
//
// The `usages`-refreshed-in-update() pattern they all shared is precisely the second declaration
// channel this brief deletes: a pass had to re-state, in a different format, what it had already
// said fluently, and only the re-statement governed correctness.

}  // namespace string::render
