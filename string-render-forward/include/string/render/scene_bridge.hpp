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

namespace string::render
{

// The ONE renderer object that reads the scene/asset layers directly (world + registry); every
// render pass consumes what it derives. It owns the renderer's GPU MIRROR of the world — the
// per-row GpuDrawInfo table (instance x mesh-part, material slots resolved, world transform
// applied) — and the skin/palette bookkeeping. It owns no scene state and no asset data: rows are
// DERIVED, rebuilt from the world snapshot whenever its revision changes.
class scene_bridge
{
public:
    // `max_rows` bounds the DrawInfo table (a spawn budget: the table is graph-invisible mapped
    // memory whose address is pushed, so it is sized once). The registry's residency gate is
    // installed HERE — the bridge owns the mapped rows the callback flips.
    scene_bridge(engine_context& ctx, ::string::assets::registry& assets, ::string::scene::world& world,
                 uint32_t max_rows);
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

    // --- skinning (brief 23 shared-window semantics: all rows of one skin share its palette
    // window; per-entity windows arrive with the per-instance skinning step) -------------------
    struct skin_instance
    {
        uint64_t skeleton_hash = 0;
        uint32_t joint_count = 0;
        uint32_t ibm_offset = 0;      // window into the registry's inverse_bind()
        uint32_t remap_offset = 0;    // window into the registry's joint_remap()
        uint32_t palette_offset = 0;  // first joint in the palette ring, mat4 units
        std::filesystem::path anim_pack;
    };
    std::span<const skin_instance> skins() const { return skins_; }
    uint32_t palette_joints_total() const { return palette_joints_total_; }

    // Latched-and-cleared: did the residency gate flip any row since the last poll? (The consumer
    // flags a visbits clear — a residency change makes those rows' meshlet visibility bits stale.)
    bool take_residency_changed()
    {
        const bool changed = residency_changed_;
        residency_changed_ = false;
        return changed;
    }

private:
    void rebuild();
    void refresh_transforms();
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

    std::vector<skin_instance> skins_;
    uint32_t palette_joints_total_ = 0;

    glm::vec3 bounds_min_{ 0.0f };
    glm::vec3 bounds_max_{ 0.0f };

    uint64_t applied_revision_ = 0;
};

}  // namespace string::render
