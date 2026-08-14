#include <string/render/scene_bridge.hpp>

#include <cstdlib>
#include <cstring>
#include <limits>

#include <string/core/logger.hpp>

namespace string::render
{

scene_bridge::scene_bridge(engine_context& ctx, ::string::assets::registry& assets,
                           ::string::scene::world& world, uint32_t max_rows)
: allocator_(ctx.allocator)
, assets_(assets)
, world_(world)
, max_rows_(std::max(max_rows, 1u))
{
    // Brief 23 shared-window palette assignment: a running sum over the registry's skins, all rows
    // of one skin sharing the window (the paperdoll shape). Assigned once — only the ring's base
    // address rotates per frame slot (through SceneData).
    skins_.reserve(assets_.skins().size());
    for (const ::string::assets::skin_binding& sb : assets_.skins())
    {
        skins_.push_back(skin_instance{
            .skeleton_hash = sb.skeleton_hash,
            .joint_count = sb.joint_count,
            .ibm_offset = sb.ibm_offset,
            .remap_offset = sb.remap_offset,
            .palette_offset = palette_joints_total_,
            .anim_pack = sb.anim_pack,
        });
        palette_joints_total_ += sb.joint_count;
    }
    if (!skins_.empty())
        STRING_LOG_INFO("[load] skins: {} instance(s), {} palette joints, {} skinned vertices",
                        skins_.size(), palette_joints_total_, assets_.skin_vertices().size());

    // The DrawInfo table: host-visible + persistent-mapped (DrawInfo.resident is the streaming
    // gate the registry's callback flips; transform updates land in place).
    draw_info_buffer_ = allocator_.create_resource(::string::gpu::buffer_info{
        .size = VkDeviceSize(sizeof(GpuDrawInfo)) * max_rows_,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
        .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                          | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
    });
    mapped_ = static_cast<GpuDrawInfo*>(
        allocator_.get_buffer(draw_info_buffer_).allocation_info.pMappedData);

    world_.tick(0.0f);   // flush so the first snapshot is coherent
    rebuild();

    // The registry's residency gate: keyed by PART index; the bridge maps it to that part's FIRST
    // row (additional instances of the same part snapshot resident at build, exactly like the old
    // crowd copies did). Installed AFTER the first rebuild so the up-front streaming loop lands on
    // real rows.
    assets_.install_residency([this](uint32_t part, uint32_t vertex_offset, uint32_t resident) {
        if (mapped_ == nullptr || part >= part_to_row_.size()) return;
        const uint32_t row = part_to_row_[part];
        if (row == ~0u || row >= active_rows_) return;
        mapped_[row].resident = resident;
        mapped_[row].vertex_offset = vertex_offset;
        residency_changed_ = true;
    });
}

scene_bridge::~scene_bridge()
{
    if (draw_info_buffer_ != 0) allocator_.destroy_resource(draw_info_buffer_);
}

void scene_bridge::build_row(GpuDrawInfo& info, row_meta& meta,
                             const ::string::scene::instance_row& inst,
                             const ::string::assets::mesh_part& part) const
{
    info = GpuDrawInfo{};
    info.lod_count = part.lod_count;
    info.first_meshlet = part.first_meshlet;
    info.total_meshlets = part.total_meshlets;
    for (uint32_t l = 0; l < ::string::asset::kMaxLods; ++l) info.lods[l] = part.lods[l];

    // World placement: the cooked part transform composed with the instance transform.
    // DrawInfo.center is WORLD-space (draw-level frustum cull + LOD select read it raw), so the
    // instance transform applies to the bounds too; radius scales by the largest axis scale.
    info.model = inst.transform * part.transform;
    const glm::vec4 c = inst.transform * glm::vec4(part.center, 1.0f);
    info.center = glm::vec3(c);
    const float sx = glm::length(glm::vec3(inst.transform[0]));
    const float sy = glm::length(glm::vec3(inst.transform[1]));
    const float sz = glm::length(glm::vec3(inst.transform[2]));
    info.radius = part.radius * std::max(sx, std::max(sy, sz));

    glm::vec3 mn(std::numeric_limits<float>::max());
    glm::vec3 mx(std::numeric_limits<float>::lowest());
    for (int i = 0; i < 8; ++i)
    {
        const glm::vec4 corner((i & 1) ? part.aabb_max.x : part.aabb_min.x,
                               (i & 2) ? part.aabb_max.y : part.aabb_min.y,
                               (i & 4) ? part.aabb_max.z : part.aabb_min.z, 1.0f);
        const glm::vec3 p = glm::vec3(inst.transform * corner);
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    }
    meta.aabb_min = mn;
    meta.aabb_max = mx;

    // Brief 23: skin binding. STRING_SKIN_OFF=1 is the kill-switch: every row renders static bind
    // pose through the unskinned load_vertex path (the identity-palette parity lever).
    static const bool skin_off = std::getenv("STRING_SKIN_OFF") != nullptr;
    const bool skinned = !skin_off && part.skin.valid();
    meta.skin = skinned ? static_cast<int32_t>(part.skin.index) : -1;
    if (skinned)
    {
        const skin_instance& si = skins_[part.skin.index];
        info.skinned = 1;
        info.skin_offset = static_cast<uint32_t>(part.skin_delta);
        info.palette_offset = si.palette_offset;
        info.joint_count = si.joint_count;
    }

    info.flags = 0;
    info.alpha_cutoff = 0.5f;
    if (part.material.valid())
    {
        using ::string::asset::CookedAlphaMode;
        const ::string::assets::material& m = assets_.material_of(part.material);
        const uint32_t white = assets_.white_slot();
        info.base_color = m.base_color_factor;
        info.base_slot = m.base_color.valid() ? assets_.texture_slot(m.base_color) : white;
        info.normal_slot =
            m.normal.valid() ? assets_.texture_slot(m.normal) : assets_.flat_normal_slot();
        info.mr_slot =
            m.metallic_roughness.valid() ? assets_.texture_slot(m.metallic_roughness) : white;
        info.metallic = m.metallic_factor;
        info.roughness = m.roughness_factor;
        info.occlusion_slot = m.occlusion.valid() ? assets_.texture_slot(m.occlusion) : white;
        if (m.alpha_mode == CookedAlphaMode::Mask)  info.flags |= kDrawFlagCutout;
        if (m.alpha_mode == CookedAlphaMode::Blend) info.flags |= kDrawFlagBlend;
        if (m.double_sided)                         info.flags |= kDrawFlagDoubleSided;
        info.alpha_cutoff = m.alpha_cutoff;
    }
    else
    {
        info.base_color = glm::vec4(1.0f);
        info.base_slot = assets_.white_slot();
        info.normal_slot = assets_.flat_normal_slot();
        info.mr_slot = assets_.white_slot();
        info.metallic = 1.0f;
        info.roughness = 1.0f;
        info.occlusion_slot = assets_.white_slot();
    }
}

void scene_bridge::rebuild()
{
    const ::string::scene::frame_view fv = world_.view();
    const std::span<const ::string::assets::mesh_part> parts = assets_.mesh_parts();

    rows_.assign(max_rows_, row_meta{});
    row_transforms_.assign(max_rows_, glm::mat4(0.0f));
    part_to_row_.assign(parts.size(), ~0u);

    // Snapshot the residency gate's last-written state per part BEFORE rewriting any row — the
    // rebuild may relocate rows, and reading a row after it was overwritten would corrupt the
    // carry-over.
    std::vector<uint32_t> prev_resident(parts.size(), 0);
    std::vector<uint32_t> prev_voffset(parts.size(), 0);
    std::vector<uint8_t> prev_valid(parts.size(), 0);
    for (uint32_t part = 0; part < part_to_row_prev_.size() && part < parts.size(); ++part)
    {
        const uint32_t prev = part_to_row_prev_[part];
        if (prev != ~0u && mapped_ != nullptr)
        {
            prev_resident[part] = mapped_[prev].resident;
            prev_voffset[part] = mapped_[prev].vertex_offset;
            prev_valid[part] = 1;
        }
    }

    glm::vec3 mn(std::numeric_limits<float>::max());
    glm::vec3 mx(std::numeric_limits<float>::lowest());
    uint32_t dst = 0;
    for (const ::string::scene::instance_row& inst : fv.instances)
    {
        if (!inst.visible) continue;
        const ::string::assets::asset* a = assets_.get(inst.asset);
        if (a == nullptr) continue;
        for (uint32_t p = 0; p < a->mesh_count(); ++p)
        {
            const uint32_t part_index = a->first_mesh().index + p;
            if (dst >= max_rows_)
            {
                STRING_LOG_WARN("[bridge] row budget exhausted ({}); remaining instances dropped",
                                max_rows_);
                break;
            }
            const ::string::assets::mesh_part& part = parts[part_index];
            GpuDrawInfo info;
            row_meta meta;
            meta.mesh = ::string::assets::mesh_id{ part_index };
            build_row(info, meta, inst, part);

            // Residency carries over: the FIRST row of a part is the registry gate's target and
            // keeps the gate's last-written state; further instances of the same part snapshot it
            // (the old crowd-copy rule).
            if (prev_valid[part_index])
            {
                info.resident = prev_resident[part_index];
                info.vertex_offset = prev_voffset[part_index];
            }
            if (part_to_row_[part_index] == ~0u) part_to_row_[part_index] = dst;

            mapped_[dst] = info;
            rows_[dst] = meta;
            row_transforms_[dst] = inst.transform;
            mn = glm::min(mn, meta.aabb_min);
            mx = glm::max(mx, meta.aabb_max);
            ++dst;
        }
    }
    active_rows_ = dst;
    part_to_row_prev_ = part_to_row_;
    if (dst == 0)
    {
        mn = glm::vec3(0.0f);
        mx = glm::vec3(0.0f);
    }
    bounds_min_ = mn;
    bounds_max_ = mx;
    applied_revision_ = fv.revision;
}

void scene_bridge::refresh_transforms()
{
    // Transform-only updates: rewrite the placement fields of rows whose instance moved. Same
    // mapped-mid-flight class as the old crowd path; a ringed row table is the moving-characters
    // follow-up.
    const ::string::scene::frame_view fv = world_.view();
    const std::span<const ::string::assets::mesh_part> parts = assets_.mesh_parts();
    uint32_t dst = 0;
    for (const ::string::scene::instance_row& inst : fv.instances)
    {
        if (!inst.visible) continue;
        const ::string::assets::asset* a = assets_.get(inst.asset);
        if (a == nullptr) continue;
        const uint32_t count = std::min(a->mesh_count(), max_rows_ - dst);
        if (std::memcmp(&row_transforms_[dst], &inst.transform, sizeof(glm::mat4)) != 0)
        {
            for (uint32_t p = 0; p < count; ++p)
            {
                const uint32_t part_index = a->first_mesh().index + p;
                const ::string::assets::mesh_part& part = parts[part_index];
                GpuDrawInfo& info = mapped_[dst + p];
                info.model = inst.transform * part.transform;
                const glm::vec4 c = inst.transform * glm::vec4(part.center, 1.0f);
                info.center = glm::vec3(c);
                const float sx = glm::length(glm::vec3(inst.transform[0]));
                const float sy = glm::length(glm::vec3(inst.transform[1]));
                const float sz = glm::length(glm::vec3(inst.transform[2]));
                info.radius = part.radius * std::max(sx, std::max(sy, sz));
                glm::vec3 mn(std::numeric_limits<float>::max());
                glm::vec3 mx(std::numeric_limits<float>::lowest());
                for (int i = 0; i < 8; ++i)
                {
                    const glm::vec4 corner((i & 1) ? part.aabb_max.x : part.aabb_min.x,
                                           (i & 2) ? part.aabb_max.y : part.aabb_min.y,
                                           (i & 4) ? part.aabb_max.z : part.aabb_min.z, 1.0f);
                    const glm::vec3 pt = glm::vec3(inst.transform * corner);
                    mn = glm::min(mn, pt);
                    mx = glm::max(mx, pt);
                }
                rows_[dst + p].aabb_min = mn;
                rows_[dst + p].aabb_max = mx;
                row_transforms_[dst + p] = inst.transform;
            }
        }
        dst += count;
    }
}

void scene_bridge::tick()
{
    const uint64_t rev = world_.view().revision;
    if (rev != applied_revision_)
    {
        rebuild();
    }
    else
    {
        refresh_transforms();
    }
}

}  // namespace string::render
