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
#include <string/core/vertex.hpp>
#include <string/vulkan/frame_graph.hpp>
#include <string/gpu/pass_context.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/scene/camera.hpp>
#include <string/gpu/pipeline.hpp>
#include "string/gpu/descriptor_allocator.hpp"
#include "string/gpu/residency_manager.hpp"
#include "string/gpu/resource.hpp"
#include "string/gpu/resource_allocator.hpp"

#include <string/scene/asset_registry.hpp>

#include <string/render/scene_bridge.hpp>
#include <string/render/lighting_data.hpp>
#include <string/render/meshlet_data.hpp>
#include <string/render/probe_gi.hpp>
#include <string/render/meshlet_builder.hpp>
#include <string/render/render_cvars.hpp>
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
// GeometryScene is DELETED (scene-layer split): LightingSettings/WorklistSet/WorklistLayout/
// DepthHistorySlot live in scene_bridge.hpp; scene state lives in the world.
class scene_uniforms;

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
    VkDeviceAddress scene;       // 112 (brief 23: SceneData — skin stream + this slot's palettes)
};
static_assert(offsetof(MeshletShadowPush, records) == 104);
static_assert(offsetof(MeshletShadowPush, scene) == 112);
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
// GeometryScene is DELETED (scene-layer split): scene state lives in the world, the shared
// per-frame surface is the bridge's scene_frame VALUE snapshot, and what remains here is the
// meshlet technique's own state.
class geometry_pass final
{
    ::string::gpu::device& device_;
    ::string::gpu::resource_allocator& allocator_;
    ::string::gpu::descriptor_table& descriptor_table_;
    string::InputMap& input_map_;
    // Froxel light binning: extracted to FroxelComponent froxel_ (declared below, brief 11).

    // Brief 20: the colour/depth sentinels are gone. The scene attachments are viewport-scaled
    // transients the application declares and hands to declare() as logical handles.

    // Content residency (geometry heaps, textures, budgets) belongs to the ASSET REGISTRY, and
    // the draw-row table to the SCENE BRIDGE (asset/scene-layer split). The pass keeps
    // references: it feeds per-frame visibility samples to the registry and consumes the bridge's
    // rows — it owns neither.
    ::string::assets::registry& assets_;
    scene_bridge& bridge_;

    // --- state that lived on the deleted GeometryScene base, now the technique's own -----------
    // Mirrors of bridge/frame state, refreshed at construction + each tick so the record bodies
    // read plain members (they run strictly after tick).
    MeshletModel meshlet_model_;   // CPU meshlet tables (dump/diagnostics; the GPU side is the registry's)
    uint32_t draw_count_ = 0;
    uint32_t base_draw_count_ = 0;
    uint32_t active_draw_count_ = 0;
    ::string::gpu::resource_id draw_info_buffer_ = 0;
    GpuDrawInfo* draw_info_mapped_ = nullptr;
    glm::vec3 scene_aabb_min_{ 0.0f };
    glm::vec3 scene_aabb_max_{ 0.0f };
    VkExtent2D screen_size{ 0, 0 };
    // Content-heap graph handles (latched from the registry's gpu_view).
    ::string::gpu::buffer vertex_buffer_{};
    ::string::gpu::buffer meshlet_buffer_{};
    ::string::gpu::buffer meshlet_vertices_{};
    ::string::gpu::buffer meshlet_triangles_{};
    ::string::gpu::buffer skin_buffer_{};
    ::string::gpu::buffer joint_palette_{};
    // Worklist format + capacity (computed in build_meshlet_gpu, published to the bridge frame).
    WorklistLayout wl_layout_;
    uint32_t cull_max_draws_ = 0;
    // Renderer debug toggles (key-driven; published to the bridge frame for the record-side
    // consumers). hiz/lod live further down with the technique state that reads them.
    bool cull_enabled_ = true;
    bool mesh_cull_frozen_ = false;
    glm::mat4 mesh_frozen_view_proj_{ 1.0f };
    glm::vec3 mesh_frozen_camera_pos_{ 0.0f };
    int debug_view_ = 0;
    bool froxel_heatmap_ = false;
    uint16_t frames_in_flight_ = 0;
    bool lookdev_ = false;
    std::shared_ptr<MeshOverlayStats> overlay_stats_;
    // The stats source (scene.upload owns the readback now); wired by the app.
    const scene_uniforms* uniforms_ = nullptr;

    // Cascaded shadow maps. A depth-only prepass (ShadowPass) renders the scene from the sun's
    // orthographic view into cascade_count per-frame-in-flight D32 images (one per cascade), sampled
    // by the lit fragment shader with 5x5 PCF. Cascades are stabilized (texel-snapped) and refit each
    // frame because the sun and camera both move (dynamic time-of-day).
    //
    // Where the state lives: sun/sky/cascade settings and the camera are in GeometryScene. Sky is
    // SkyPass (SkyComponent), drawn first in the scene MSAA group. Sky IBL is IblPass, published as
    // scene().ibl — GeometryPass reads sh_address/env/dfg slots for SceneData and probe GI reads the
    // SH. Probe GI is GiPass (scene().gi); shadow maps are ShadowPass (scene().shadow).
    // Recompute the per-cascade stabilized ortho fits from the live camera + sun (called per frame).

    // --- Forward+ lighting: scene data + local lights + froxels --------------------------------

    // The persistent per-meshlet visibility bitfield physical (graph handle minted in declare).
    ::string::gpu::resource_id visbits_buffer_ = 0;
    // Scratch reused each frame: the visibility samples fed to the registry (frustum-visible parts
    // + their projected screen coverage — the registry turns coverage into texture mip wants).
    std::vector<::string::assets::visibility_sample> visibility_scratch_;
    // Frames since scene start (drives the two-phase warm-up gate + the periodic cull logs; the
    // registry keeps its own equivalent for streaming).
    uint64_t stream_frame_ = 0;

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
    // Computed by GtaoPass from the PREVIOUS frame slot's min-resolved depth
    // (hz.depth) under that frame's matrices; consumed by this frame's lit fragments through
    // SceneData.prev_view_proj reprojection (exact for the static scene; 1-frame-late AO, zero
    // temporal accumulation so nothing can ghost). raw -> denoise -> per-slot final (RGBA8:
    // rgb bent world normal, a visibility). Requires the two-phase HiZ depth; auto-off otherwise.
    // GTAO targets/slots/sampler/programs owned by GtaoPass (GtaoChain component).

    // Brief 06: address of the renderer's Tracy GPU context (from engine_context), for finer per-stage
    // GPU zones inside the pass bodies. Null when the renderer exposes none; the zone macros
    // are no-ops without -Dtracy regardless. Read live at record time (the ctx is created after the
    // pass is built). Helper below turns the double-indirection into the value the macros want.
    STRING_PROFILE_GPU_CONTEXT_TYPE* gpu_profiler_ctx_ = nullptr;
    STRING_PROFILE_GPU_CONTEXT_TYPE gpu_ctx() const
    {
        STRING_PROFILE_GPU_CONTEXT_TYPE none{};
        return gpu_profiler_ctx_ ? *gpu_profiler_ctx_ : none;
    }

    // Runtime state.
    bool hiz_enabled_ = true;        // O toggles two-pass HiZ occlusion on the meshlet path
    bool lod_enabled_ = true;        // (LOD select on by default)
    // (dbg.orbit moved to the app: it drives world.camera() now.)
    // Crowd stress scene (K): the base draws duplicated across a grid with varied transforms, to
    // prove 500+-crowd geometry throughput (brief M6). Crowd draws are extra DrawInfo entries that
    // reference the SAME meshlet buffers (only the transform differs); active_draw_count_ switches
    // between the base set and base+crowd. Grid side N gives (N*N - 1) extra copies of the model.
    // The crowd stress scene is CONTENT (world entities) now: the K toggle / r.crowd.enabled call
    // the app-installed hook, which spawns/despawns the grid entities; the bridge re-derives rows.
    bool crowd_enabled_ = false;
    bool crowd_seeded_ = false;
    std::function<void(bool)> crowd_hook_;
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
    uint32_t visbits_words_ = 0;
    // Per-draw LOD selected LAST frame (device-local, one word per draw). The draw-cull compute
    // compares it to this frame's selected LOD; a switch clears that draw's bitfield range (its
    // meshlets become "new" -> phase 2), because bits are keyed to the CURRENT LOD's meshlet ids.
    ::string::gpu::resource_id prev_draw_lod_buffer_ = 0;
    // True when the two-phase path is active this frame (HiZ enabled + warmup done). When false the
    // pass renders single-pass (phase 0) into one render group.
    bool two_phase_active_ = false;
    // Records phase-1 or phase-2 opaque+two-sided draws into the (already-open) MSAA render pass.
    void record_opaque_phase(::string::pass_context& ctx, uint32_t phase,
                             ::string::gpu::image pyramid,
                             ::string::gpu::buffer scene_data, ::string::gpu::buffer stats);
    // Zeroes visbits_buffer_ + prev_draw_lod_buffer_ (teleport/first-frame/streaming full clear).
    bool visbits_clear_pending_ = true;


public:
    // Latest GPU culling stats (read back one frame late by scene.upload) for the UI overlay.
    const GpuMeshStats& mesh_stats() const;

    // The app's crowd hook (spawn/despawn the stress-scene entities in the world).
    void set_crowd_hook(std::function<void(bool)> hook) { crowd_hook_ = std::move(hook); }

    // Ring sizing: the palette-ring joint BUDGET (mat4 units per frame slot; per-instance
    // windows are the bridge's).
    uint32_t palette_joints_total() const { return bridge_.palette_joints_total(); }
    // `assets` is the app-owned asset registry with this scene's content already loaded (the app
    // loads models / generated content before constructing the pass); the pass renders EVERY mesh
    // part in it, in registry order. The registry must outlive the pass — the geometry streamer
    // uploads from a non-owning view of its vertex heap. `overlay_stats` (may be null) receives
    // the meshlet culling stats + path/HiZ/view flags each frame for the UI overlay.
    // `lookdev` (brief 07): the standing material-probe preset — kills the local-light stress set
    // by default so material response reads under sun + sky IBL alone (content itself is loaded by
    // the app via sandbox content tools). STRING_SCENE=lookdev ./run.sh.
    geometry_pass(string::engine_context& context, VkSampleCountFlagBits samples,
                  ::string::assets::registry& assets, scene_bridge& bridge,
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
                 string::gpu::buffer ibl_sh, string::gpu::buffer froxels,
                 // Brief 23: the app-backed joint-palette ring (one physical per frame slot, CPU
                 // written by the anim tick). Invalid handle = scene never skins.
                 string::gpu::buffer joint_palette = {});

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
    // One-shot zero of every declared work list (see record_reset_lists). Transients are
    // single-backed, so this is one flag, not one per frame slot.
    bool lists_zeroed_ = false;
    // MSAA sample count of the scene attachments. The APP declares those, so the app states this;
    // the renderer no longer has an opinion to supply.
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
    void record_phase1(string::pass_context& ctx);
    // The three per-frame resets, one declared pass each (see declare()).
    void record_reset_lists(string::pass_context& ctx);
    void record_reset_stats(string::pass_context& ctx);
    void record_reset_visbits(string::pass_context& ctx);
    void record_phase2(string::pass_context& ctx);
    bool two_phase_active() const { return two_phase_active_; }
    // The stats source for the overlay publish (scene.upload owns the readback now); app-wired.
    void set_uniforms(const scene_uniforms* uniforms) { uniforms_ = uniforms; }
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

}  // namespace string::render
