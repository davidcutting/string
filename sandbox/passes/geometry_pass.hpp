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
#include <string/vulkan/pass_context.hpp>
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
#include "meshlet_builder.hpp"
#include "assetbake/scene_loader.hpp"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <volk.h>

namespace sandbox
{

// Push constant for the froxel light-binning compute (matches Push in shaders/froxel_cull.slang).
struct FroxelPush
{
    glm::mat4 view;
    glm::mat4 inv_proj;
    glm::uvec2 screen;
    glm::uvec2 grid;
    uint32_t slices;
    uint32_t tile_size;
    float near_plane;
    float far_plane;
    uint32_t light_count;
    uint32_t max_per_froxel;
    uint32_t _pad0;
    uint32_t _pad1;
    VkDeviceAddress lights;
    VkDeviceAddress froxels;
};

// CVar-able lighting/shadow/froxel quality constants, grouped so a future CVar system binds them in
// one place (per the brief's "as CVars" intent).
struct LightingSettings
{
    uint32_t cascade_count = 3;                 // stabilized CSM cascades (quality tier)
    uint32_t shadow_resolution = 2048;          // per-cascade shadow map resolution
    float cascade_split_lambda = 0.85f;         // practical-split blend (log vs uniform)
    float cascade_blend = 0.12f;                // cross-fade band as a fraction of a cascade's far
    float shadow_bias = 0.0009f;                // constant depth bias (reverse-Z units), re-tuned
    float shadow_normal_offset_scale = 3.0f;    // multiplier on per-cascade world texel size
    float shadow_depth_range = 200.0f;          // world depth the CSM covers from the camera
};

// Push constant for the procedural sky background (sky.vert/frag): the inverse view-projection (to
// turn NDC into a world ray), the camera position, and the sky/sun colours shared with the lit
// shader's image-based ambient. Matches the Push block in sky.frag (vec3s on 16-byte boundaries).
struct SkyPush
{
    glm::mat4 inv_view_proj;
    glm::vec3 camera_pos;  float furnace;   // brief 07: 1 -> uniform white background
    glm::vec3 sun_dir;     float _sp1;
    glm::vec3 sky_zenith;  float _sp2;
    glm::vec3 sky_ground;  float _sp3;           // ground ALBEDO (radiance derived in-shader)
    glm::vec3 sun_color;   float sun_intensity;  // klx perpendicular
};

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

// Push for the brief-07 IBL compute chain (matches Push in shaders/ibl.slang; std430: float4s at
// 0/16/32/48, scalars from 64, the 8-byte pointer 8-aligned at 96).
struct IblPush
{
    glm::vec4 sun_dir;       // xyz sun dir; w furnace flag (capture writes uniform white)
    glm::vec4 sky_zenith;
    glm::vec4 sky_ground;
    glm::vec4 sun_color;
    uint32_t src_slot;       // 64
    uint32_t dst_slot;       // 68
    uint32_t dst_size;       // 72
    uint32_t src_size;       // 76
    float roughness;         // 80  prefilter: PERCEPTUAL roughness of this ladder mip
    uint32_t sample_count;   // 84
    uint32_t mip_count;      // 88  capture chain mips (prefilter PDF lod clamp)
    uint32_t _pad0;          // 92
    VkDeviceAddress sh;      // 96  ShBuffer address (sh_project)
};
static_assert(offsetof(IblPush, src_slot) == 64);
static_assert(offsetof(IblPush, sh) == 96);
static_assert(sizeof(IblPush) == 104);

// Renders a whole glTF model: uploads its shared vertex/index buffers plus every texture (each
// into a bindless slot), then issues one indexed draw per node-instanced primitive, pushing the
// primitive's transform and material inline. Content-agnostic — the model path is supplied by
// the application, resolved under context.resources_path.
class GeometryPass final : public String::Pass
{
    string::gpu::device& device_;
    string::gpu::resource_allocator& allocator_;
    string::gpu::descriptor_table& descriptor_table_;
    String::InputMap& input_map_;
    // Froxel light binning is a hot-reloadable Slang program (brief 01 registry); the pass binds
    // ->current() each frame.
    string::gpu::shader_program* froxel_program_ = nullptr;

    string::gpu::resource_id vertex_buffer_;
    uint32_t draw_count_ = 0;
    uint32_t frames_in_flight_ = 1;
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
    LightingSettings settings_;
    glm::vec3 sun_dir_ = glm::normalize(glm::vec3(0.5f, 0.72f, 0.45f));  // direction TO the light
    // Per-cascade fit, recomputed each frame in update() from the live camera + sun.
    std::array<glm::mat4, kMaxCascades> cascade_view_proj_{};
    std::array<float, kMaxCascades> cascade_split_{};       // view-space far distance per cascade
    std::array<float, kMaxCascades> cascade_world_texel_{}; // world units per texel per cascade
    // Brief 04 M4: each cascade's world-space bounding sphere (centre + depth-inflated radius) for the
    // conservative draw-level shadow cull in the draw-cull compute.
    std::array<glm::vec3, kMaxCascades> cascade_center_{};
    std::array<float, kMaxCascades> cascade_cull_radius_{};
    // Time-of-day: an angle animated (or scrubbed) that drives sun_dir_ + the sky colours.
    float time_of_day_ = 0.30f;   // 0..1 across the day arc (0.30 ~= mid-morning)
    bool sun_animate_ = false;    // T toggles continuous time-of-day advance

    // Environment (procedural sky + image-based ambient). The sky colours drive BOTH the visible sky
    // background and the lit shader's ambient, so shaded surfaces read as lit by the same sky.
    // Brief 07 M4 units: radiance in kilo-nits, illuminance in kilolux (see sun_for_time).
    glm::vec3 sky_zenith_ = glm::vec3(2.8f, 6.0f, 12.4f);    // clear-day zenith blue (knits)
    // Ground band ALBEDO (reflectance, not radiance): the shader derives its radiance from the
    // CURRENT sun + sky (sky_ground_radiance in sky.slang) so the ground tracks time of day.
    // Calibrated so noon (t=0.5) reproduces the pre-fix constant (2.64, 2.28, 1.80) knits.
    glm::vec3 sky_ground_ = glm::vec3(0.0824f, 0.0699f, 0.0503f);
    glm::vec3 sun_color_ = glm::vec3(1.0f, 0.96f, 0.9f);
    float sun_intensity_ = 100.0f;                           // klx perpendicular (noon)
    // Fullscreen procedural sky, drawn before geometry. Ported to Slang: the pipeline is owned by
    // the hot-reload registry (recompiles + swaps on save); the pass binds sky_program_->current().
    string::gpu::shader_program* sky_program_ = nullptr;

    // --- Brief 07: dynamic sky IBL ---------------------------------------------------------------
    // The live sky is captured to a small cubemap, GGX-prefiltered into a roughness ladder and
    // SH-projected — all on the GPU, re-run only when the sun moves past a threshold (amortized;
    // <0.3 ms budget). Products: env_prefiltered_ (SamplerCube ladder), sh_buffer_ (9 float4 E/pi
    // coefficients), dfg_lut_ (split-sum BRDF LUT, baked once). Single-buffered: the chain records
    // at the top of the frame's main-queue command buffer, ordered against last frame's fragment
    // reads by the intra-pass barriers (documented local-barrier class, like the HiZ mip chain).
    static constexpr uint32_t kEnvSize = 128;           // capture / prefilter face size
    static constexpr uint32_t kEnvCaptureMips = 6;      // capture average chain (PDF mips + SH source)
    static constexpr uint32_t kEnvPrefilterMips = 6;    // roughness ladder 128..4 (r = mip/(mips-1))
    static constexpr uint32_t kShSourceMip = 3;         // 16x16 capture mip the SH projects from
    static constexpr uint32_t kDfgSize = 128;
    static constexpr uint32_t kPrefilterSamples = 64;
    static constexpr uint32_t kDfgSamples = 1024;
    string::gpu::resource_id env_capture_ = 0;          // cube, RGBA16F, kEnvCaptureMips
    string::gpu::resource_id env_prefiltered_ = 0;      // cube, RGBA16F, kEnvPrefilterMips
    string::gpu::resource_id dfg_lut_ = 0;              // 2D RGBA16F (rg used)
    string::gpu::resource_id sh_buffer_ = 0;            // 9 x float4, device-local
    uint32_t env_capture_sample_slot_ = 0;              // SamplerCube (prefilter source)
    uint32_t env_prefiltered_slot_ = 0;                 // SamplerCube (shading)
    uint32_t dfg_sample_slot_ = 0;                      // Sampler2D (shading)
    uint32_t dfg_storage_slot_ = 0;
    std::vector<VkImageView> env_capture_mip_views_;    // per-mip 2D_ARRAY storage views
    std::vector<uint32_t> env_capture_mip_slots_;
    std::vector<VkImageView> env_prefiltered_mip_views_;
    std::vector<uint32_t> env_prefiltered_mip_slots_;
    VkSampler env_sampler_ = VK_NULL_HANDLE;            // linear, clamp, mip-linear (env + LUT)
    string::gpu::shader_program* env_capture_program_ = nullptr;
    string::gpu::shader_program* env_mip_program_ = nullptr;
    string::gpu::shader_program* env_prefilter_program_ = nullptr;
    string::gpu::shader_program* sh_project_program_ = nullptr;
    string::gpu::shader_program* dfg_program_ = nullptr;
    bool dfg_baked_ = false;
    bool ibl_layouts_initialized_ = false;   // env images moved UNDEFINED -> GENERAL once
    bool ibl_primed_ = false;                // at least one capture recorded
    bool ibl_update_pending_ = false;        // update() trigger -> record_compute runs the chain
    glm::vec3 ibl_captured_sun_dir_{ 0.0f };
    bool ibl_captured_furnace_ = false;
    uint64_t ibl_update_count_ = 0;          // instrumentation (amortization honesty)
    bool furnace_ = false;                   // r.furnace (white-furnace acceptance test)
    bool lookdev_ = false;                   // material-probe scene (STRING_SCENE=lookdev)
    void create_ibl_resources(String::PassContext& context);
    void record_ibl_update(VkCommandBuffer cb);
    // dbg.ibl_verify: reads the DFG LUT + SH coefficients back and checks them against CPU
    // references (M1 numeric gate). Stalls the device; debug only.
    void run_ibl_verification();
    // Shadow depth images: [frame_in_flight][cascade]. One D32 map per cascade per frame in flight,
    // so frame N+1's shadow render doesn't race N's sample and each cascade has its own map.
    std::vector<std::array<string::gpu::resource_id, kMaxCascades>> shadow_images_;
    std::vector<std::array<uint32_t, kMaxCascades>> shadow_slots_;
    VkSampler shadow_sampler_ = VK_NULL_HANDLE;             // nearest + clamp, for manual PCF
    // Recompute the per-cascade stabilized ortho fits from the live camera + sun (called per frame).
    void compute_cascades();

    // --- Forward+ lighting: scene data + local lights + froxels --------------------------------
    // Per-frame SceneData SSBO (device-addressed, persistent-mapped ring): all the lighting/shadow/
    // froxel state the lit shader reads that overflows the push constant.
    std::vector<string::gpu::resource_id> scene_buffers_;
    std::vector<void*> scene_mapped_;
    // Dynamic local lights (point + spot). Uploaded to a per-frame SSBO ring so the stress scene can
    // animate them CPU-side each frame.
    std::vector<GpuLight> lights_;
    std::vector<string::gpu::resource_id> light_buffers_;
    std::vector<void*> light_mapped_;
    // Per-froxel light-index lists, written by the froxel_cull compute and read by the lit shader.
    // One per frame in flight (compute overwrites it each frame).
    std::vector<string::gpu::resource_id> froxel_buffers_;
    uint32_t froxel_tiles_x_ = 0;
    uint32_t froxel_tiles_y_ = 0;
    uint32_t froxel_count_ = 0;
    uint32_t froxel_capacity_ = 0;   // allocated froxel count (grows with the screen)
    // (Re)allocate the per-frame froxel index buffers for the current screen size if it grew.
    void ensure_froxel_capacity();
    // Light stress scene: hundreds of moving colored lights orbiting over the model (test bed).
    void build_light_stress_scene(const glm::vec3& aabb_min, const glm::vec3& aabb_max);
    void animate_lights(float delta_time);
    struct LightAnim { glm::vec3 center; float radius; float speed; float phase; float height; };
    std::vector<LightAnim> light_anim_;
    glm::vec3 scene_aabb_min_{ 0.0f };
    glm::vec3 scene_aabb_max_{ 0.0f };
    bool lights_enabled_ = true;   // L toggles the local-light stress set
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

    std::vector<GltfMaterial> materials_;
    std::vector<GltfDraw> draws_;

    // Reusable engine fly camera (glTF space, Y-up). Framed to the model's AABB at load; update()
    // drives it through the InputMap's default fly controls + mouse-look.
    String::Camera camera_;

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
    MeshletModel meshlet_model_;
    string::gpu::resource_id meshlet_buffer_ = 0;      // GpuMeshlet[]
    string::gpu::resource_id meshlet_vertices_ = 0;    // uint[] global vertex remap
    string::gpu::resource_id meshlet_triangles_ = 0;   // uint[] packed local tris
    string::gpu::resource_id draw_info_buffer_ = 0;    // GpuDrawInfo[] (host-visible; resident gate)
    GpuDrawInfo* draw_info_mapped_ = nullptr;
    // GPU-written per-frame stats, read back one frame later for the UI overlay + log line. One
    // buffer per frame-in-flight (host-visible) so the readback doesn't stall the GPU.
    std::vector<string::gpu::resource_id> stats_buffers_;
    std::vector<GpuMeshStats> stats_readback_;         // last read stats per frame slot
    GpuMeshStats stats_latest_{};                       // most recent, for the UI accessor
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
    String::FrameScratch* scratch_ = nullptr;
    bool scratch_bound_ = false;
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
    void record_draw_cull(VkCommandBuffer cb, uint16_t current_frame, int cascade = -1);
    // Compaction phase for one worklist (scan + fill): compacts surviving draws into commands[]+records[]
    // + count, ready for one vkCmdDrawMeshTasksIndirectCountEXT. `draw_lod` is the per-draw selected-LOD
    // buffer the records[] inherit (camera-selected for camera+shadow lists, zeroed for the LOD0 prepass).
    void record_expand(VkCommandBuffer cb, const Worklist& wl, VkDeviceAddress draw_lod);

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
    void record_transparency(VkCommandBuffer cb, uint16_t current_frame, uint32_t count);

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
    VkSampler hiz_sampler_ = VK_NULL_HANDLE;
    uint32_t hiz_screen_w_ = 0, hiz_screen_h_ = 0;
    void ensure_hiz(uint16_t current_frame);

    // --- Brief 09: half-res GTAO with bent normals ----------------------------------------------
    // Computed at the top of record_compute from the PREVIOUS frame slot's min-resolved depth
    // (hz.depth) under that frame's matrices; consumed by this frame's lit fragments through
    // SceneData.prev_view_proj reprojection (exact for the static scene; 1-frame-late AO, zero
    // temporal accumulation so nothing can ghost). raw -> denoise -> per-slot final (RGBA8:
    // rgb bent world normal, a visibility). Requires the two-phase HiZ depth; auto-off otherwise.
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
    void record_gtao(VkCommandBuffer cb, uint16_t current_frame);

    // Brief 06: address of the renderer's Tracy GPU context (from PassContext), for finer per-stage
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
    uint32_t base_draw_count_ = 0;   // draws in the base (non-crowd) scene
    uint32_t active_draw_count_ = 0; // base, or base + crowd when the crowd is on
    void build_crowd(const glm::vec3& aabb_min, const glm::vec3& aabb_max);
    void build_meshlet_gpu(String::PassContext& context);
    // Brief 04d: `phase` (0 legacy/transparency, 1 = bit-set only, 2 = bit-clear+HiZ+update) is pushed
    // into MeshletPush so the task shader partitions the worklist's meshlets across the two phases.
    void record_meshlet_draws(VkCommandBuffer cb, const string::gpu::pipeline& p,
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
    void record_opaque_phase(VkCommandBuffer cb, uint16_t current_frame, uint32_t phase);
    // Zeroes visbits_buffer_ + prev_draw_lod_buffer_ (teleport/first-frame/streaming full clear).
    bool visbits_clear_pending_ = true;

    // Shared overlay state written each frame for the UI (stats + which path is active), so the UI
    // author (built separately in the RenderPlan) can display it without a direct pass pointer.
    std::shared_ptr<MeshOverlayStats> overlay_stats_;

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
    GeometryPass(String::PassContext& context, std::vector<std::filesystem::path> model_paths,
                 std::shared_ptr<MeshOverlayStats> overlay_stats = nullptr, bool lookdev = false);
    virtual ~GeometryPass() override;

    // Stable identity for tooling (Tracy zones, inspector). Brief 06. The renderer names the
    // pass-level GPU/CPU zones from this; finer per-stage zones (draw-cull, HiZ, shadows, sky,
    // transparency) are scoped inside record_compute/record via STRING_PROFILE_GPU_ZONE.
    std::string_view debug_name() const override { return "geometry"; }

    virtual void update(float delta_time, uint16_t current_frame) override;
    virtual bool record_compute(string::gpu::command_recorder& recorder, uint16_t current_frame) override;
    virtual void record(string::gpu::command_recorder& recorder, uint16_t current_frame) override;

    // Brief 04e M4: froxel light binning is the pass's dependency-free async-compute chain
    // (declared via async_usages; the renderer picks the lane or records inline).
    virtual bool has_async_compute() const override;
    virtual void record_async_compute(string::gpu::command_recorder& recorder, uint16_t current_frame) override;

    // Brief 04d two-phase occlusion. record() draws PHASE 1 (sky + last-frame-visible opaque, into the
    // MSAA targets). The renderer then MIN-resolves the MSAA depth into hz.depth, calls record_between()
    // to build the HiZ pyramid, and record_after_between() draws PHASE 2 (the disocclusion complement +
    // transparency) into the reloaded MSAA targets. When HiZ is disabled the pass does NOT break the
    // group: record() renders everything single-pass (phase 0) as before.
    virtual bool breaks_scene_group() const override { return two_phase_active_; }
    virtual string::gpu::resource_id depth_resolve_target(uint16_t current_frame) const override;
    virtual void record_between(string::gpu::command_recorder& recorder, uint16_t current_frame) override;
    virtual void record_after_between(string::gpu::command_recorder& recorder, uint16_t current_frame) override;
};

}  // namespace sandbox
