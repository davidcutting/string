#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <string/gpu/resource.hpp>
#include <string/scene/camera.hpp>

#include <string/asset/cooked_format.hpp>   // brief 23: SkinVertex (the compact skin heap's element)
#include <string/scene/asset.hpp>           // assets::material / assets::mesh_part (runtime tables)

#include <string/render/lighting_data.hpp>
#include <string/render/geometry/sky_component.hpp>
#include <string/render/geometry/froxel_component.hpp>
#include <string/render/geometry/ibl_component.hpp>
#include <string/render/meshlet_builder.hpp>
#include <string/render/meshlet_data.hpp>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

namespace string::render
{

class froxel_component;   // app-owned (brief 20); GeometryScene holds a non-owning ptr.
class ibl_component;      // app-owned; GeometryScene holds a non-owning ptr.
class shadow_maps;        // app-owned; GeometryScene holds a non-owning ptr.
class gtao_chain;         // app-owned; GeometryScene holds a non-owning ptr.
class probe_gi_component; // app-owned; GeometryScene holds a non-owning ptr.

// One frame slot's MIN-resolved scene depth, plus the camera it was rendered with. Shared because it
// is the engine's DEPTH HISTORY: the resolve (geometry.phase1's group) writes it, the HiZ build reads
// this frame's, and temporal effects read a PREVIOUS frame's through the matrices captured alongside
// it. GTAO's reprojection is the first such consumer; anything temporal added later (TAA, SSR) wants
// exactly this. Rendering uses the LIVE camera even under freeze-cull, so these are the true
// depth-buffer transforms, not the (possibly frozen) cull ones.
struct DepthHistorySlot
{
    ::string::gpu::resource_id image = 0;   // the slot's resolved single-sample depth
    uint32_t bindless_slot = 0;             // its sampled descriptor slot
    uint8_t valid = 0;                      // has this slot ever been resolved into?
    glm::mat4 view{ 1.0f };
    glm::mat4 proj{ 1.0f };
    glm::mat4 view_proj{ 1.0f };
};

// The GPU work lists: one graph-owned TRANSIENT BUFFER each (brief 21 D4), holding that list's
// draw-cull counts, scan scratch, compacted indirect commands + records, and surviving-draw count at
// the WorklistLayout offsets below.
//
// They used to be byte regions of one per-frame-slot arena (FrameScratch), which cost two things the
// graph is supposed to provide: every list aliased into one allocation, so the graph could only see
// "the arena" and had to serialise the camera and cascade expand chains against each other; and the
// arena's lifetime was hand-managed outside the graph, with a materialize-then-rebind dance that the
// first frames raced. As separate declarations the chains de-serialise and the aliasing decision goes
// back to where it belongs — materialize()'s interval packing.
//
// The set is passed around whole because the culler fills lists it does not own: one dispatch writes
// the camera lists, one per cascade writes that cascade's.
struct WorklistSet
{
    ::string::gpu::buffer opaque;
    ::string::gpu::buffer twosided;
    std::array<::string::gpu::buffer, kMaxCascades> cascade{};
    ::string::gpu::buffer draw_lod;   // the shared per-draw selected LOD, one word per draw
};

// Byte layout WITHIN one worklist region. Computed once during the meshlet build and shared, because
// the producer (the cull/expand computes) and the consumers (the camera draws, the cascade draws)
// must agree on it, and they no longer live in the same class.
struct WorklistLayout
{
    VkDeviceSize offsets_off = 0;    // per-draw scan offsets[]
    VkDeviceSize blocksums_off = 0;  // block-carry sums
    VkDeviceSize commands_off = 0;   // compacted indirect commands[] (12B each)
    VkDeviceSize records_off = 0;    // parallel records[] ({draw_index, LOD}, 8B each)
    VkDeviceSize count_off = 0;      // surviving-draw count word
    VkDeviceSize size = 0;           // total bytes per worklist region
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

// Brief 11 Phase 2: the shared "scene" state that every geometry sub-domain reads — the geometry/
// draw tables, the per-frame SceneData ring + local lights, the camera + environment/CSM lighting
// state, the frame scratch arena, and the culling stats. Extracted out of the GeometryPass
// god-object so it is a standalone, referenceable type: GeometryPass privately inherits it today
// (the TU-split record methods keep accessing these fields unqualified), and the independent graph
// passes M2 promotes will each take a GeometryScene& instead of being methods on one class.
//
// Technique-PRIVATE working state (HiZ pyramids, GTAO buffers, probe-GI atlases, worklists,
// transparency lists, shader-program handles, streaming/residency, debug toggles) stays on
// GeometryPass — it belongs to one sub-pass, not the shared scene.
struct GeometryScene
{
    // Brief 20: the per-frame parameter blocks the sky, froxel and IBL components latch in their
    // tick(). They are built HERE because every field already lives here — this is the shared scene
    // state those components read. Assembling them at the call site would mean the app reaching into
    // renderer internals to restate what the scene already knows.
    SkyParams sky_params() const;
    FroxelParams froxel_params() const;
    IblLighting ibl_lighting() const;

    // --- Geometry / draw tables ------------------------------------------------------------------
    // The material/part tables live in the ASSET REGISTRY and the draw rows in the SCENE BRIDGE
    // now; what remains here are the shared counts + handles every technique reads.
    uint32_t draw_count_ = 0;
    uint32_t base_draw_count_ = 0;   // draws in the base (non-crowd) scene
    uint32_t active_draw_count_ = 0; // base, or base + crowd when the crowd is on
    glm::vec3 scene_aabb_min_{ 0.0f };
    glm::vec3 scene_aabb_max_{ 0.0f };

    // The CPU meshlet tables (still pass-adopted copies, transitional) + the per-draw DrawInfo
    // table. The GPU heaps themselves belong to the ASSET REGISTRY now — these are its graph
    // handles (assets::gpu_view), latched at construction; every consumer declares its own read
    // and resolves the address through its pass_context (no allocator escape).
    MeshletModel meshlet_model_;
    ::string::gpu::buffer meshlet_buffer_{};      // GpuMeshlet[]
    ::string::gpu::buffer meshlet_vertices_{};    // uint[] global vertex remap
    ::string::gpu::buffer meshlet_triangles_{};   // uint[] packed local tris
    ::string::gpu::resource_id draw_info_buffer_ = 0;    // GpuDrawInfo[] (host-visible; resident gate)
    GpuDrawInfo* draw_info_mapped_ = nullptr;

    // --- Camera ----------------------------------------------------------------------------------
    // Reusable engine fly camera (glTF space, Y-up). Framed to the model's AABB at load; update()
    // drives it through the InputMap's default fly controls + mouse-look.
    // Current viewport. Was on the deleted Pass base; it is scene state, not a graph concept.
    VkExtent2D screen_size{ 0, 0 };
    string::Camera camera_;

    // --- Cascaded shadow maps + environment lighting ---------------------------------------------
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
    bool furnace_ = false;                   // r.furnace (white-furnace acceptance test)
    bool lookdev_ = false;                   // material-probe scene (STRING_SCENE=lookdev)

    // --- Forward+ lighting: per-frame SceneData ring + local lights ------------------------------
    // Brief 16 M1: the resource-virtualization hub (set from engine_context in the GeometryPass ctor).
    // The per-frame SceneData ring + (M3) the light/stats rings are registry-owned handles resolved
    // through this per frame, replacing the hand-managed [frame] vectors of resource_ids.
    // Brief 20: the ResourceRegistry is deleted. Buffers that used to resolve through it are graph
    // resources now; a pass resolves them through pass_context at record time.

    // Per-frame SceneData SSBO (device-addressed, persistent-mapped ring): all the lighting/shadow/
    // froxel state the lit shader reads that overflows the push constant. Now a PerFrame registry
    // buffer — resolve the address/mapped pointer via resources->{address,mapped}(scene_buffer_, slot).
    ::string::gpu::buffer scene_buffer_;
    // Dynamic local lights (point + spot). Uploaded to a per-frame SSBO ring so the stress scene can
    // animate them CPU-side each frame.
    std::vector<GpuLight> lights_;
    // Brief 16 M3: registry-owned PerFrame ring (was light_buffers_/light_mapped_ [frame] vectors).
    ::string::gpu::buffer light_buffer_;
    bool lights_enabled_ = true;   // L toggles the local-light stress set (shared: SceneData + froxel)
    // Brief 11 step 3: the froxel light-binning is its own FroxelPass now; it publishes its component
    // here (non-owning) so GeometryPass can read the froxel grid dims + buffer address for SceneData.
    froxel_component* froxel = nullptr;
    // Brief 11 step 3: the dynamic sky IBL is its own IblPass (prepass compute); published here so
    // GeometryPass reads the SH address + shading slots for SceneData, and probe GI reads the SH.
    ibl_component* ibl = nullptr;

    // --- Culling stats + frame infrastructure ----------------------------------------------------
    uint32_t frames_in_flight_ = 1;
    // GPU-written per-frame stats, read back one frame later for the UI overlay + log line. A
    // registry-owned PerFrame host-visible ring (brief 16 M3; was stats_buffers_) so the readback
    // doesn't stall the GPU.
    ::string::gpu::buffer stats_buffer_;
    std::vector<GpuMeshStats> stats_readback_;         // last read stats per frame slot
    GpuMeshStats stats_latest_{};                       // most recent, for the UI accessor
    // Shared worklist format + cull capacity: the cull/expand computes (GeometryPass) write lists that
    // ShadowMaps consumes, so both sides need the layout and the max-draw bound — and the APP needs
    // them to size the declarations (worklist_bytes / draw_lod_bytes).
    WorklistLayout wl_layout_;
    uint32_t cull_max_draws_ = 0;
    // The vertex heap's graph handle (registry-owned backing). In the scene tables (not
    // GeometryPass) because the cascade draws push its device address too, like the meshlet heaps.
    ::string::gpu::buffer vertex_buffer_{};

    // --- Brief 23 skinning ------------------------------------------------------------------------
    // The skin tables live in the asset registry and the palette windows in the scene bridge now.
    // The compact skin heap's graph handle (registry-owned backing, whole-uploaded at load, never
    // suballocated — DrawInfo.skin_offset rebases ORIGINAL global indices, so geometry streaming
    // does not touch it).
    ::string::gpu::buffer skin_buffer_{};
    // The app-backed palette ring's graph handle (declare() stores it; scene.upload publishes this
    // slot's address into SceneData).
    ::string::gpu::buffer joint_palette_{};
    // Cascaded shadow maps + their per-cascade work lists, owned by ShadowPass. Published here so
    // GeometryPass's SceneData can read the bindless cascade slots and the culler can fill the lists.
    shadow_maps* shadow = nullptr;
    // Half-res GTAO + bent normals, owned by GtaoPass. Published so SceneData can read the AO slot
    // and extent, and so the go/no-go decision has one home.
    gtao_chain* gtao = nullptr;
    // Relightable probe-GI volume + atlases, owned by GiPass.
    probe_gi_component* gi = nullptr;
    // Per-frame-slot resolved depth + the camera it was rendered with (see DepthHistorySlot).
    std::vector<DepthHistorySlot> depth_history_;

    // --- Meshlet cull view state -----------------------------------------------------------------
    // Shared because EVERY meshlet draw path derives its cull inputs from these — the opaque phases
    // and the sorted transparency draw alike. Freeze-cull in particular must freeze every camera-
    // dependent input together (frustum planes, cone-cull eye, HiZ nearest-point, LOD select); a
    // partial freeze mixes frozen and live inputs and produces bogus culling as the camera moves away.
    bool cull_enabled_ = true;       // debug: GPU frustum culling off entirely (C)
    bool mesh_cull_frozen_ = false;  // meshlet freeze-cull (F keybind)
    glm::mat4 mesh_frozen_view_proj_{ 1.0f };
    glm::vec3 mesh_frozen_camera_pos_{ 0.0f };
    int debug_view_ = 0;             // 0 none, 1 meshlet colour, 2 LOD colour, 3 occlusion reject (V)
    // Persistent per-meshlet visibility bitfield (1 bit per GLOBAL meshlet id), device-local. Owned by
    // the two-phase occlusion path but named by every meshlet push constant — the transparency draw
    // passes a valid pointer with freeze_bits set rather than a null one.
    ::string::gpu::resource_id visbits_buffer_ = 0;
    // Shared overlay state written each frame for the UI (stats + which path is active), so the UI
    // author (built separately in the RenderPlan) can display it without a direct pass pointer.
    std::shared_ptr<MeshOverlayStats> overlay_stats_;
};

}  // namespace string::render
