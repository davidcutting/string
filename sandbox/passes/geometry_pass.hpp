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
    glm::vec3 camera_pos;  float _sp0;
    glm::vec3 sun_dir;     float _sp1;
    glm::vec3 sky_zenith;  float _sp2;
    glm::vec3 sky_ground;  float _sp3;
    glm::vec3 sun_color;   float _sp4;
};

// Push constant for the task/mesh meshlet draw path (matches Push in shaders/meshlet_mesh.slang;
// std430 push-constant layout — offsets verified against %Push_std430 OpMemberDecorate). 240B; fits
// RDNA3's 256B push budget. Two mat4 (view_proj + cull source), eight device addresses, then scalars.
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
    VkDeviceAddress records;     // 184 (brief 03b: {draw_index, lod} per indirect draw, SV_DrawIndex)
    glm::vec3 camera_pos;        // 192
    float _pad_cp;               // 204
    uint32_t debug_view;         // 208
    uint32_t hiz_slot;           // 212
    uint32_t hiz_mips;           // 216
    uint32_t _pad_hiz;           // 220 (std430: uvec2 hiz_size is 8-aligned -> pad to 224)
    glm::uvec2 hiz_size;         // 224
};
static_assert(sizeof(MeshletPush) == 232);
static_assert(offsetof(MeshletPush, records) == 184);
static_assert(offsetof(MeshletPush, camera_pos) == 192);
static_assert(offsetof(MeshletPush, debug_view) == 208);
static_assert(offsetof(MeshletPush, hiz_size) == 224);

// Push for the depth-only meshlet shadow path (matches Push in shaders/meshlet_shadow.slang).
struct MeshletShadowPush
{
    glm::mat4 light_view_proj;   // 0
    VkDeviceAddress vertices;    // 64
    VkDeviceAddress meshlets;    // 72
    VkDeviceAddress mverts;      // 80
    VkDeviceAddress mtris;       // 88
    VkDeviceAddress draws;       // 96
    VkDeviceAddress records;     // 104 (brief 03b: {draw_index, lod} per indirect draw, SV_DrawIndex)
    uint32_t _pad0;              // 112
    uint32_t _pad1;              // 116
};
static_assert(offsetof(MeshletShadowPush, records) == 104);
static_assert(sizeof(MeshletShadowPush) == 120);

// Push for the GPU draw-cull compute (matches Push in shaders/meshlet_draw_cull.slang; std430).
struct DrawCullPush
{
    glm::mat4 cull_view_proj;        // 0   draw-level frustum source (frozen-aware; C disables)
    VkDeviceAddress draws;           // 64
    VkDeviceAddress commands;        // 72  frustum-culled list (main pass / HiZ prepass)
    VkDeviceAddress shadow_commands; // 80  resident-only list (cascades: casters may be off-camera)
    VkDeviceAddress records;         // 88
    VkDeviceAddress count;           // 96
    VkDeviceAddress stats;           // 104
    uint32_t draw_count;             // 112
    uint32_t lod_enabled;            // 116
    uint32_t frustum_cull;           // 120 (0 = C toggle off -> pass everything)
    uint32_t stats_lod;              // 124 (1 = write the LOD histogram; camera pass only)
    glm::vec3 camera_pos;            // 128 (16-aligned; frozen-aware — drives LOD select)
    float lod_error_px;              // 140
    float focal;                     // 144
    uint32_t _pad0;                  // 148
    uint32_t _pad1;                  // 152
    uint32_t _pad2;                  // 156
};
static_assert(offsetof(DrawCullPush, draws) == 64);
static_assert(offsetof(DrawCullPush, camera_pos) == 128);
static_assert(offsetof(DrawCullPush, lod_error_px) == 140);
static_assert(sizeof(DrawCullPush) == 160);

// Push for the HiZ pyramid downsample (matches Push in shaders/hiz_build.slang).
struct HizPush
{
    uint32_t src_slot;
    uint32_t dst_slot;
    glm::uvec2 dst_size;
    glm::uvec2 src_size;
    uint32_t copy_depth;
    uint32_t _pad;
};

// Push for the per-frame stats reset (matches Push in shaders/meshlet_reset.slang).
struct ResetPush
{
    VkDeviceAddress stats;
    uint32_t stats_words;
    uint32_t _pad;
};

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
    string::gpu::resource_id index_buffer_;
    uint32_t draw_count_ = 0;
    uint32_t frames_in_flight_ = 1;

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
    // Time-of-day: an angle animated (or scrubbed) that drives sun_dir_ + the sky colours.
    float time_of_day_ = 0.30f;   // 0..1 across the day arc (0.30 ~= mid-morning)
    bool sun_animate_ = false;    // T toggles continuous time-of-day advance

    // Environment (procedural sky + image-based ambient). The sky colours drive BOTH the visible sky
    // background and the lit shader's ambient, so shaded surfaces read as lit by the same sky.
    glm::vec3 sky_zenith_ = glm::vec3(0.14f, 0.30f, 0.62f);  // clear-day zenith blue (linear)
    glm::vec3 sky_ground_ = glm::vec3(0.22f, 0.19f, 0.15f);  // warm ground/bounce
    glm::vec3 sun_color_ = glm::vec3(1.0f, 0.96f, 0.9f);
    float sun_intensity_ = 3.0f;
    // Fullscreen procedural sky, drawn before geometry. Ported to Slang: the pipeline is owned by
    // the hot-reload registry (recompiles + swaps on save); the pass binds sky_program_->current().
    string::gpu::shader_program* sky_program_ = nullptr;
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
    string::gpu::shader_program* meshlet_shadow_program_ = nullptr;
    string::gpu::shader_program* hiz_program_ = nullptr;
    string::gpu::shader_program* reset_program_ = nullptr;
    // Brief 03b: GPU draw-cull compute. One thread per draw appends an indirect mesh-task command +
    // a {draw_index, lod} record to a per-frame work list the CPU issues with a single
    // vkCmdDrawMeshTasksIndirectCountEXT (main pass + prepass + 3 shadow cascades).
    string::gpu::shader_program* draw_cull_program_ = nullptr;
    // Per-frame-in-flight work lists (the GPU may still read last frame's list). Each buffer packs:
    // commands[max_draws] (VkDrawMeshTasksIndirectCommandEXT, 12B) at offset 0, records[max_draws]
    // ({draw_index,lod}, 8B) at offset kRecordsOffset, then a single uint count at kCountOffset.
    // Two lists per frame: the camera list (selected LOD; main pass + shadow cascades reuse it) and
    // the prepass list (forced LOD0; the HiZ depth prepass draws the fullest silhouette).
    std::vector<string::gpu::resource_id> cull_cmd_buffers_;         // camera list
    std::vector<string::gpu::resource_id> cull_prepass_buffers_;     // LOD0 prepass list
    uint32_t cull_max_draws_ = 0;
    VkDeviceSize cull_shadow_cmds_offset_ = 0; // byte offset of the resident-only shadow_cmds[]
    VkDeviceSize cull_records_offset_ = 0;   // byte offset of records[] within a work-list buffer
    VkDeviceSize cull_count_offset_ = 0;     // byte offset of the count uint
    // Dispatch the draw-cull compute for one work list (mode chooses LOD-forcing + histogram write).
    void record_draw_cull(VkCommandBuffer cb, uint16_t current_frame, bool prepass);

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
        // Single-sample depth the pyramid's mip 0 reduces from. The renderer's shared depth target is
        // 4x MSAA (can't be sampled with a plain Sampler2D), so the meshlet path renders its own
        // single-sample depth-only camera prepass here each frame, then builds the pyramid from it.
        string::gpu::resource_id depth = 0;
        uint32_t depth_slot = 0;                       // sampled slot of the prepass depth
    };
    std::vector<HizPyramid> hiz_;
    VkSampler hiz_sampler_ = VK_NULL_HANDLE;
    uint32_t hiz_screen_w_ = 0, hiz_screen_h_ = 0;
    void ensure_hiz(uint16_t current_frame);

    // Runtime state.
    bool meshlet_readback_pending_ = false;  // STRING_MESHLET_READBACK: GPU-vs-CPU memcmp at frame 50
    bool hiz_enabled_ = true;        // O toggles two-pass HiZ occlusion on the meshlet path
    bool lod_enabled_ = true;        // (LOD select on by default)
    int debug_view_ = 0;             // 0 none, 1 meshlet colour, 2 LOD colour, 3 occlusion reject (V cycles)
    bool mesh_cull_frozen_ = false;  // meshlet freeze-cull (F keybind)
    glm::mat4 mesh_frozen_view_proj_{ 1.0f };
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
    void record_meshlet_draws(VkCommandBuffer cb, const string::gpu::pipeline& p,
                              uint16_t current_frame);

    // Shared overlay state written each frame for the UI (stats + which path is active), so the UI
    // author (built separately in the RenderPlan) can display it without a direct pass pointer.
    std::shared_ptr<MeshOverlayStats> overlay_stats_;

public:
    // Latest GPU culling stats (read back one frame late) for the UI overlay.
    const GpuMeshStats& mesh_stats() const { return stats_latest_; }
    // model_path is a .gltf/.glb resolved relative to context.resources_path. `overlay_stats` (may be
    // null) receives the meshlet culling stats + path/HiZ/view flags each frame for the UI overlay.
    GeometryPass(String::PassContext& context, const std::filesystem::path& model_path,
                 std::shared_ptr<MeshOverlayStats> overlay_stats = nullptr);
    virtual ~GeometryPass() override;

    virtual void update(float delta_time, uint16_t current_frame) override;
    virtual bool record_compute(string::gpu::command_recorder& recorder, uint16_t current_frame) override;
    virtual void record(string::gpu::command_recorder& recorder, uint16_t current_frame) override;
};

}  // namespace sandbox
