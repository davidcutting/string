#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <string/gpu/resource.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/vulkan/engine_context.hpp>

#include <string/scene/asset_registry.hpp>
#include <string/scene/world.hpp>

#include <string/render/meshlet_data.hpp>
#include <string/render/lighting_data.hpp>
#include <string/render/geometry/sky_component.hpp>
#include <string/render/geometry/froxel_component.hpp>
#include <string/render/geometry/ibl_component.hpp>

#include <array>

namespace string::render
{

// CVar-able lighting/shadow/froxel quality constants (moved from GeometryScene with its deletion).
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

// The GPU work lists: one graph-owned TRANSIENT BUFFER each (brief 21 D4). Passed around whole
// because the culler fills lists it does not own: one dispatch writes the camera lists, one per
// cascade writes that cascade's. (Moved from GeometryScene.)
struct WorklistSet
{
    ::string::gpu::buffer opaque;
    ::string::gpu::buffer twosided;
    std::array<::string::gpu::buffer, kMaxCascades> cascade{};
    ::string::gpu::buffer draw_lod;   // the shared per-draw selected LOD, one word per draw
};

// Byte layout WITHIN one worklist region. Computed once during the meshlet build and shared,
// because the producer (the cull/expand computes) and the consumers (the camera draws, the
// cascade draws) must agree on it. (Moved from GeometryScene.)
struct WorklistLayout
{
    VkDeviceSize offsets_off = 0;    // per-draw scan offsets[]
    VkDeviceSize blocksums_off = 0;  // block-carry sums
    VkDeviceSize commands_off = 0;   // compacted indirect commands[] (12B each)
    VkDeviceSize records_off = 0;    // parallel records[] ({draw_index, LOD}, 8B each)
    VkDeviceSize count_off = 0;      // surviving-draw count word
    VkDeviceSize size = 0;           // total bytes per worklist region
};

// One frame slot's MIN-resolved scene depth, plus the camera it was rendered with — the engine's
// DEPTH HISTORY (GTAO reprojection; any temporal effect wants exactly this). (Moved from
// GeometryScene; the bridge owns the vector, the geometry pass writes it as it resolves.)
struct DepthHistorySlot
{
    ::string::gpu::resource_id image = 0;   // the slot's resolved single-sample depth
    uint32_t bindless_slot = 0;             // its sampled descriptor slot
    uint8_t valid = 0;                      // has this slot ever been resolved into?
    glm::mat4 view{ 1.0f };
    glm::mat4 proj{ 1.0f };
    glm::mat4 view_proj{ 1.0f };
};

// The per-frame value snapshot every render pass consumes — what the mutable, shared
// GeometryScene base class used to be, republished as DERIVED DATA each tick. Field names keep
// the old member spellings deliberately (the consumers' reads port 1:1).
struct scene_frame
{
    // View (from the world's camera) + presentation extent.
    glm::mat4 view{ 1.0f };
    glm::mat4 view_proj{ 1.0f };
    glm::vec3 camera_pos{ 0.0f };
    float near_plane = 0.1f;
    float far_plane = 100.0f;
    float fov_degrees = 60.0f;
    float aspect = 1.0f;
    VkExtent2D screen_size{ 0, 0 };

    ::string::scene::environment env;
    std::span<const GpuLight> lights;
    glm::vec3 scene_aabb_min_{ 0.0f };
    glm::vec3 scene_aabb_max_{ 0.0f };

    // Cascade fit (bridge-computed from camera + sun + settings each tick).
    glm::mat4 cascade_view_proj_[kMaxCascades]{};
    float cascade_split_[kMaxCascades]{};
    float cascade_world_texel_[kMaxCascades]{};
    glm::vec3 cascade_center_[kMaxCascades]{};
    float cascade_cull_radius_[kMaxCascades]{};

    // Draw-table state (bridge-owned).
    uint32_t draw_count_ = 0;
    uint32_t base_draw_count_ = 0;
    uint32_t active_draw_count_ = 0;
    ::string::gpu::resource_id draw_info_buffer_ = 0;
    GpuDrawInfo* draw_info_mapped_ = nullptr;

    // Content-heap graph handles (the registry's gpu_view) + the app's palette ring handle.
    ::string::gpu::buffer vertex_buffer_{};
    ::string::gpu::buffer meshlet_buffer_{};
    ::string::gpu::buffer meshlet_vertices_{};
    ::string::gpu::buffer meshlet_triangles_{};
    ::string::gpu::buffer skin_buffer_{};
    ::string::gpu::buffer joint_palette_{};

    // Renderer-shared technique state (published by the geometry pass at build/tick).
    WorklistLayout wl_layout_;
    uint32_t cull_max_draws_ = 0;
    ::string::gpu::resource_id visbits_buffer_ = 0;
    LightingSettings settings_;
    bool cull_enabled_ = true;
    bool mesh_cull_frozen_ = false;
    glm::mat4 mesh_frozen_view_proj_{ 1.0f };
    glm::vec3 mesh_frozen_camera_pos_{ 0.0f };
    int debug_view_ = 0;
    bool froxel_heatmap_ = false;
};

// The ONE renderer object that reads the scene/asset layers directly (world + registry); every
// render pass consumes what it derives. It owns the renderer's GPU MIRROR of the world — the
// per-row GpuDrawInfo table (instance x mesh-part, material slots resolved, world transform
// applied) — and the skin/palette bookkeeping. It owns no scene state and no asset data: rows are
// DERIVED, rebuilt from the world snapshot whenever its revision changes.
class scene_bridge
{
public:
    // `max_rows` bounds the DrawInfo table and `palette_budget_joints` the joint-palette ring —
    // both SPAWN BUDGETS (the table's address is pushed and the ring is app-backed, so both are
    // sized once). The registry's residency gate is installed HERE — the bridge owns the mapped
    // rows the callback flips.
    scene_bridge(engine_context& ctx, ::string::assets::registry& assets, ::string::scene::world& world,
                 uint32_t max_rows, uint32_t palette_budget_joints);
    ~scene_bridge();

    scene_bridge(const scene_bridge&) = delete;
    scene_bridge& operator=(const scene_bridge&) = delete;

    // Re-derive rows from the world snapshot: full rebuild when the row SET changed (spawn/
    // despawn/visibility), in-place model/bounds refresh when only transforms moved. Call once per
    // frame, after world.tick and before the passes' ticks.
    void tick();

    // Row-level meta for the renderer's CPU paths (frustum cull, streaming feedback, inspector).
    struct row_meta
    {
        ::string::assets::mesh_id mesh;         // registry part (streaming feedback key)
        glm::vec3 aabb_min{ 0.0f };             // world-space (instance transform applied)
        glm::vec3 aabb_max{ 0.0f };
        int32_t skin = -1;                      // index into skins(), -1 = static
    };
    std::span<const row_meta> rows() const { return { rows_.data(), active_rows_ }; }
    uint32_t row_count() const { return active_rows_; }
    uint32_t max_rows() const { return max_rows_; }

    ::string::gpu::resource_id draw_info_buffer() const { return draw_info_buffer_; }
    GpuDrawInfo* mapped() const { return mapped_; }

    // Scene world bounds (union of row AABBs) — camera framing, GI volume, cascade depth.
    glm::vec3 bounds_min() const { return bounds_min_; }
    glm::vec3 bounds_max() const { return bounds_max_; }

    // --- skinning: PER-INSTANCE palette windows ------------------------------------------------
    // Each spawned skinned entity gets its own window per skin, allocated from the ring budget at
    // rebuild (deterministic row order; the whole budget re-packs on any spawn/despawn, which is
    // safe because every allocated window is fully rewritten each frame). Two instances of one
    // skinned character finally animate independently — impossible under the old load-time
    // shared-window running sum. Budget exhaustion degrades those rows to bind pose with one WARN.
    uint32_t palette_joints_total() const { return palette_budget_; }

    // Build every allocated window's palettes into THIS frame slot's mapped ring: the world's
    // animators supply model-space poses (already sampled in world::tick); windows without a
    // valid animator are written IDENTITY (bind pose — windows re-pack across rebuilds, so a
    // stale occupant's data must never show through). Call after tick(), before the render.
    void write_palettes(std::span<glm::mat4> ring_slot);

    // Latched-and-cleared: did the residency gate flip any row since the last poll? (The consumer
    // flags a visbits clear — a residency change makes those rows' meshlet visibility bits stale.)
    bool take_residency_changed()
    {
        const bool changed = residency_changed_;
        residency_changed_ = false;
        return changed;
    }

    // --- the per-frame snapshot every pass consumes ---------------------------------------------
    const scene_frame& frame() const { return frame_; }
    LightingSettings& settings() { return frame_.settings_; }
    const LightingSettings& settings() const { return frame_.settings_; }
    // The app's palette-ring graph handle, latched after it declares its resources.
    void set_joint_palette(::string::gpu::buffer h) { frame_.joint_palette_ = h; }
    // The geometry technique publishes its shared state (worklist layout + visbits) at build...
    void set_worklists(const WorklistLayout& layout, uint32_t cull_max_draws,
                       ::string::gpu::resource_id visbits)
    {
        frame_.wl_layout_ = layout;
        frame_.cull_max_draws_ = cull_max_draws;
        frame_.visbits_buffer_ = visbits;
    }
    // ...and its cull-debug toggles per tick (the transparency pass + scene.upload read them).
    void set_cull_debug(bool cull_enabled, bool frozen, const glm::mat4& frozen_view_proj,
                        const glm::vec3& frozen_camera_pos, int debug_view, bool froxel_heatmap)
    {
        frame_.cull_enabled_ = cull_enabled;
        frame_.mesh_cull_frozen_ = frozen;
        frame_.mesh_frozen_view_proj_ = frozen_view_proj;
        frame_.mesh_frozen_camera_pos_ = frozen_camera_pos;
        frame_.debug_view_ = debug_view;
        frame_.froxel_heatmap_ = froxel_heatmap;
    }

    // The engine's depth history (written by the geometry pass as it resolves; read by GTAO's
    // reprojection and the scene-uniforms prev_view_proj).
    std::vector<DepthHistorySlot>& depth_history() { return depth_history_; }
    const std::vector<DepthHistorySlot>& depth_history() const { return depth_history_; }

private:
    void rebuild();
    void refresh_transforms();
    void refresh_frame();
    void compute_cascades();
    void build_row(GpuDrawInfo& info, row_meta& meta, const ::string::scene::instance_row& inst,
                   const ::string::assets::mesh_part& part) const;

    ::string::gpu::resource_allocator& allocator_;
    ::string::assets::registry& assets_;
    ::string::scene::world& world_;

    uint32_t max_rows_ = 0;
    uint32_t active_rows_ = 0;
    ::string::gpu::resource_id draw_info_buffer_ = 0;
    GpuDrawInfo* mapped_ = nullptr;
    std::vector<row_meta> rows_;
    std::vector<glm::mat4> row_transforms_;      // last-applied instance transform per row
    std::vector<uint32_t> part_to_row_;          // part -> FIRST row (residency gate target)
    std::vector<uint32_t> part_to_row_prev_;     // previous build's mapping (resident carry-over)
    bool residency_changed_ = false;

    // Per-(instance, skin) palette windows, re-packed at every rebuild.
    struct palette_job
    {
        uint32_t entity_index = 0;
        ::string::assets::skin_id skin{};
        uint32_t window = 0;   // first joint in the ring, mat4 units
        uint32_t joints = 0;
    };
    uint32_t palette_budget_ = 0;
    uint32_t palette_cursor_ = 0;
    std::vector<palette_job> palette_jobs_;
    bool palette_warned_ = false;

    glm::vec3 bounds_min_{ 0.0f };
    glm::vec3 bounds_max_{ 0.0f };

    scene_frame frame_;
    std::vector<DepthHistorySlot> depth_history_;

    uint64_t applied_revision_ = 0;
};

// Assemble the per-frame parameter blocks the sky, froxel and IBL components latch in tick().
// Every field is scene state the frame already carries; this is assembly, not policy. (Moved from
// GeometryScene with its deletion — free functions so the components stay frame-agnostic.)
SkyParams sky_params(const scene_frame& f);
FroxelParams froxel_params(const scene_frame& f);
IblLighting ibl_lighting(const scene_frame& f);

}  // namespace string::render
