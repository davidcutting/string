#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <string/gpu/resource.hpp>
#include <string/gpu/resource_registry.hpp>
#include <string/scene/camera.hpp>
#include <string/vulkan/frame_scratch.hpp>

#include "../gltf_loader.hpp"
#include "../lighting_data.hpp"
#include "../meshlet_builder.hpp"
#include "../meshlet_data.hpp"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

namespace sandbox
{

class FroxelComponent;   // owned by FroxelPass (brief 11 step 3); GeometryScene holds a non-owning ptr.
class IblComponent;      // owned by IblPass (brief 11 step 3); GeometryScene holds a non-owning ptr.

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
    // --- Geometry / draw tables ------------------------------------------------------------------
    std::vector<GltfMaterial> materials_;
    std::vector<GltfDraw> draws_;
    uint32_t draw_count_ = 0;
    uint32_t base_draw_count_ = 0;   // draws in the base (non-crowd) scene
    uint32_t active_draw_count_ = 0; // base, or base + crowd when the crowd is on
    glm::vec3 scene_aabb_min_{ 0.0f };
    glm::vec3 scene_aabb_max_{ 0.0f };

    // Built at load from the flattened geometry: the GPU-side meshlet/vertex/triangle heaps + the
    // per-draw DrawInfo table. Metadata buffers are small and always resident; only the vertex/index
    // heaps stream (DrawInfo.resident is the streaming gate).
    MeshletModel meshlet_model_;
    string::gpu::resource_id meshlet_buffer_ = 0;      // GpuMeshlet[]
    string::gpu::resource_id meshlet_vertices_ = 0;    // uint[] global vertex remap
    string::gpu::resource_id meshlet_triangles_ = 0;   // uint[] packed local tris
    string::gpu::resource_id draw_info_buffer_ = 0;    // GpuDrawInfo[] (host-visible; resident gate)
    GpuDrawInfo* draw_info_mapped_ = nullptr;

    // --- Camera ----------------------------------------------------------------------------------
    // Reusable engine fly camera (glTF space, Y-up). Framed to the model's AABB at load; update()
    // drives it through the InputMap's default fly controls + mouse-look.
    String::Camera camera_;

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
    string::gpu::ResourceRegistry* resources = nullptr;
    // Per-frame SceneData SSBO (device-addressed, persistent-mapped ring): all the lighting/shadow/
    // froxel state the lit shader reads that overflows the push constant. Now a PerFrame registry
    // buffer — resolve the address/mapped pointer via resources->{address,mapped}(scene_buffer_, slot).
    string::gpu::buffer scene_buffer_;
    // Dynamic local lights (point + spot). Uploaded to a per-frame SSBO ring so the stress scene can
    // animate them CPU-side each frame.
    std::vector<GpuLight> lights_;
    // Brief 16 M3: registry-owned PerFrame ring (was light_buffers_/light_mapped_ [frame] vectors).
    string::gpu::buffer light_buffer_;
    bool lights_enabled_ = true;   // L toggles the local-light stress set (shared: SceneData + froxel)
    // Brief 11 step 3: the froxel light-binning is its own FroxelPass now; it publishes its component
    // here (non-owning) so GeometryPass can read the froxel grid dims + buffer address for SceneData.
    FroxelComponent* froxel = nullptr;
    // Brief 11 step 3: the dynamic sky IBL is its own IblPass (prepass compute); published here so
    // GeometryPass reads the SH address + shading slots for SceneData, and probe GI reads the SH.
    IblComponent* ibl = nullptr;

    // --- Culling stats + frame infrastructure ----------------------------------------------------
    uint32_t frames_in_flight_ = 1;
    // GPU-written per-frame stats, read back one frame later for the UI overlay + log line. A
    // registry-owned PerFrame host-visible ring (brief 16 M3; was stats_buffers_) so the readback
    // doesn't stall the GPU.
    string::gpu::buffer stats_buffer_;
    std::vector<GpuMeshStats> stats_readback_;         // last read stats per frame slot
    GpuMeshStats stats_latest_{};                       // most recent, for the UI accessor
    // Brief 04e M3: per-frame transient buffers reserve into the renderer's scratch arena.
    string::gpu::FrameScratch* scratch_ = nullptr;
    bool scratch_bound_ = false;
    // Shared overlay state written each frame for the UI (stats + which path is active), so the UI
    // author (built separately in the RenderPlan) can display it without a direct pass pointer.
    std::shared_ptr<MeshOverlayStats> overlay_stats_;
};

}  // namespace sandbox
