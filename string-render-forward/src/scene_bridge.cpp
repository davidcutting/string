#include <string/render/scene_bridge.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

#include <string/core/logger.hpp>

namespace string::render
{

scene_bridge::scene_bridge(engine_context& ctx, ::string::assets::registry& assets,
                           ::string::scene::world& world, uint32_t max_rows,
                           uint32_t palette_budget_joints)
: allocator_(ctx.allocator)
, assets_(assets)
, world_(world)
, max_rows_(std::max(max_rows, 1u))
, palette_budget_(palette_budget_joints)
{
    if (!assets_.skins().empty())
        STRING_LOG_INFO("[load] skins: {} in the registry, palette budget {} joints, {} skinned "
                        "vertices; per-instance windows (anim.clip / anim.blend / anim.rate / "
                        "anim.demo control playback)",
                        assets_.skins().size(), palette_budget_, assets_.skin_vertices().size());

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
    refresh_frame();     // counts/handles/bounds valid before any consumer constructs

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

    // (Skin binding is per-INSTANCE and assigned in rebuild(), where the window is allocated.)

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

    // Per-instance palette windows re-pack from zero every rebuild (safe: every allocated window
    // is fully rewritten each frame by write_palettes, identity when no animator).
    palette_cursor_ = 0;
    palette_jobs_.clear();
    static const bool skin_off = std::getenv("STRING_SKIN_OFF") != nullptr;

    glm::vec3 mn(std::numeric_limits<float>::max());
    glm::vec3 mx(std::numeric_limits<float>::lowest());
    uint32_t dst = 0;
    for (const ::string::scene::instance_row& inst : fv.instances)
    {
        if (!inst.visible) continue;
        const ::string::assets::asset* a = assets_.get(inst.asset);
        if (a == nullptr) continue;
        // This INSTANCE's windows, one per skin of its asset, allocated on first skinned part.
        struct inst_window { uint32_t skin_index; uint32_t window; };
        std::vector<inst_window> windows;
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

            // Brief 23 skin binding, per INSTANCE now. STRING_SKIN_OFF=1 is the kill-switch:
            // every row renders static bind pose through the unskinned load_vertex path (the
            // identity-palette parity lever).
            if (!skin_off && part.skin.valid())
            {
                uint32_t window = ~0u;
                for (const inst_window& w : windows)
                    if (w.skin_index == part.skin.index) { window = w.window; break; }
                const ::string::assets::skin_binding& sb = assets_.skin_of(part.skin);
                if (window == ~0u)
                {
                    if (palette_cursor_ + sb.joint_count <= palette_budget_)
                    {
                        window = palette_cursor_;
                        palette_cursor_ += sb.joint_count;
                        windows.push_back({ part.skin.index, window });
                        palette_jobs_.push_back(palette_job{ .entity_index = inst.entity_index,
                                                             .skin = part.skin,
                                                             .window = window,
                                                             .joints = sb.joint_count });
                    }
                    else if (!palette_warned_)
                    {
                        palette_warned_ = true;
                        STRING_LOG_WARN("[bridge] palette budget exhausted ({} joints); further "
                                        "skinned instances render bind pose",
                                        palette_budget_);
                    }
                }
                if (window != ~0u)
                {
                    meta.skin = static_cast<int32_t>(part.skin.index);
                    info.skinned = 1;
                    info.skin_offset = static_cast<uint32_t>(part.skin_delta);
                    info.palette_offset = window;
                    info.joint_count = sb.joint_count;
                }
            }

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
    refresh_frame();
}

void scene_bridge::write_palettes(std::span<glm::mat4> ring_slot)
{
    const std::span<const glm::mat4> ibm = assets_.inverse_bind();
    const std::span<const uint32_t> remap = assets_.joint_remap();
    for (const palette_job& job : palette_jobs_)
    {
        if (job.window + job.joints > ring_slot.size()) continue;
        const ::string::assets::skin_binding& sb = assets_.skin_of(job.skin);
        const ::string::scene::animator* match = nullptr;
        for (const std::unique_ptr<::string::scene::animator>& a :
             world_.animators_at(job.entity_index))
        {
            if (a != nullptr && a->skin() == job.skin && a->valid())
            {
                match = a.get();
                break;
            }
        }
        const std::span<glm::mat4> window = ring_slot.subspan(job.window, job.joints);
        if (match != nullptr && !match->model_space().empty())
        {
            ::string::anim::build_palette(match->model_space(),
                                          remap.subspan(sb.remap_offset, sb.joint_count),
                                          ibm.subspan(sb.ibm_offset, sb.joint_count), window);
        }
        else
        {
            // No (valid) animator: bind pose. Written EVERY frame because windows re-pack across
            // rebuilds — a stale occupant's pose must never show through.
            std::fill(window.begin(), window.end(), glm::mat4(1.0f));
        }
    }
}

// Re-derive the published per-frame snapshot: the world's view/environment/lights, the bridge's
// own table state + heap handles, then the cascade fit (camera + sun + settings).
void scene_bridge::refresh_frame()
{
    const ::string::scene::frame_view fv = world_.view();
    frame_.view = fv.view;
    frame_.view_proj = fv.view_proj;
    frame_.camera_pos = fv.camera_pos;
    frame_.near_plane = fv.near_plane;
    frame_.far_plane = fv.far_plane;
    frame_.fov_degrees = fv.fov_degrees;
    frame_.aspect = fv.aspect;
    frame_.screen_size = { fv.viewport_width, fv.viewport_height };
    frame_.env = fv.env;
    frame_.lights = fv.lights;
    frame_.scene_aabb_min_ = bounds_min_;
    frame_.scene_aabb_max_ = bounds_max_;

    // The three counts collapsed into one when the crowd became content: rows ARE the draw set.
    frame_.draw_count_ = active_rows_;
    frame_.base_draw_count_ = active_rows_;
    frame_.active_draw_count_ = active_rows_;
    frame_.draw_info_buffer_ = draw_info_buffer_;
    frame_.draw_info_mapped_ = mapped_;

    const ::string::assets::gpu_view& gv = assets_.view();
    frame_.vertex_buffer_ = gv.vertices;
    frame_.meshlet_buffer_ = gv.meshlets;
    frame_.meshlet_vertices_ = gv.meshlet_vertices;
    frame_.meshlet_triangles_ = gv.meshlet_triangles;
    frame_.skin_buffer_ = gv.skin_stream;

    if (frame_.draw_count_ > 0) compute_cascades();
}

// Practical split scheme (Zhang) + sphere-stabilized, texel-snapped ortho fits. Moved verbatim
// from geometry_pass (it is DERIVED state: camera + sun + settings + scene bounds in, cascade
// matrices out — nothing about it was the geometry technique's).
void scene_bridge::compute_cascades()
{
    const uint32_t count = std::min(frame_.settings_.cascade_count, kMaxCascades);
    const float near_clip = frame_.near_plane;
    const float far_clip = std::min(frame_.settings_.shadow_depth_range, frame_.far_plane);
    const float range = far_clip - near_clip;

    const glm::mat4 inv_cam = glm::inverse(frame_.view_proj);
    const glm::vec3 L = glm::normalize(frame_.env.sun_dir);   // direction TO the sun

    float last_split = near_clip;
    for (uint32_t c = 0; c < count; ++c)
    {
        const float p = static_cast<float>(c + 1) / static_cast<float>(count);
        const float log_split = near_clip * std::pow(far_clip / near_clip, p);
        const float uniform_split = near_clip + range * p;
        const float split = frame_.settings_.cascade_split_lambda * log_split
                          + (1.0f - frame_.settings_.cascade_split_lambda) * uniform_split;
        frame_.cascade_split_[c] = split;

        // The 8 corners of this cascade's sub-frustum, mapped from NDC (reverse-Z: near=1, far=0)
        // into world via the inverse camera view-projection, then re-scaled so its near/far match
        // this slice's [last_split, split] view-space depths.
        glm::vec3 corners[8];
        int ci = 0;
        for (int x = 0; x < 2; ++x)
            for (int y = 0; y < 2; ++y)
                for (int z = 0; z < 2; ++z)
                {
                    const glm::vec4 ndc(x ? 1.0f : -1.0f, y ? 1.0f : -1.0f, z ? 1.0f : 0.0f, 1.0f);
                    const glm::vec4 w = inv_cam * ndc;
                    corners[ci++] = glm::vec3(w) / w.w;
                }
        // March along the frustum edge FROM the near corner TOWARD the far corner; the lerp
        // parameter divides by the CAMERA's full depth (see the cascade history in git).
        const float cam_range = frame_.far_plane - near_clip;
        for (int i = 0; i < 4; ++i)
        {
            const glm::vec3 far_c = corners[i * 2 + 0];   // z=0 -> camera far plane
            const glm::vec3 near_c = corners[i * 2 + 1];  // z=1 -> camera near plane
            const glm::vec3 edge = far_c - near_c;        // near -> far along this frustum edge
            corners[i * 2 + 0] = near_c + edge * ((split - near_clip) / cam_range);
            corners[i * 2 + 1] = near_c + edge * ((last_split - near_clip) / cam_range);
        }

        glm::vec3 center(0.0f);
        for (const glm::vec3& corner : corners) center += corner;
        center /= 8.0f;

        float radius = 0.0f;
        for (const glm::vec3& corner : corners) radius = std::max(radius, glm::length(corner - center));
        radius = std::ceil(radius * 16.0f) / 16.0f;   // quantize the radius so it doesn't jitter

        const float texel = (2.0f * radius) / static_cast<float>(frame_.settings_.shadow_resolution);
        frame_.cascade_world_texel_[c] = texel;

        // Depth range: bracket the SCENE AABB along the light axis (a BOUNDED extent), NOT the
        // cascade radius — the true caster..receiver span.
        float min_proj = std::numeric_limits<float>::max();
        float max_proj = std::numeric_limits<float>::lowest();
        for (int k = 0; k < 8; ++k)
        {
            const glm::vec3 corner((k & 1) ? frame_.scene_aabb_max_.x : frame_.scene_aabb_min_.x,
                                   (k & 2) ? frame_.scene_aabb_max_.y : frame_.scene_aabb_min_.y,
                                   (k & 4) ? frame_.scene_aabb_max_.z : frame_.scene_aabb_min_.z);
            const float d = glm::dot(corner - center, L);   // signed distance along L (toward sun = +)
            min_proj = std::min(min_proj, d);
            max_proj = std::max(max_proj, d);
        }
        const float margin = 1.0f;
        const float eye_back = max_proj + margin;
        const float far_d = (max_proj - min_proj) + 2.0f * margin;
        const glm::vec3 up = std::abs(L.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);

        // Stabilize: snap the cascade center to whole shadow texels in a FIXED light basis
        // (rotation only — measuring in the per-frame view space is a no-op; see git history).
        const glm::mat3 light_basis = glm::mat3(glm::lookAt(glm::vec3(0.0f), -L, up));
        glm::vec3 center_ls = light_basis * center;
        center_ls.x = std::floor(center_ls.x / texel) * texel;
        center_ls.y = std::floor(center_ls.y / texel) * texel;
        const glm::vec3 snapped_center = glm::transpose(light_basis) * center_ls;
        const glm::vec3 snapped_eye = snapped_center + L * eye_back;
        const glm::mat4 view = glm::lookAt(snapped_eye, snapped_center, up);

        // orthoRH_ZO explicitly (NOT glm::ortho): the [-1,1] default box breaks the reverse-Z
        // remap below and hard-clips the sunward half of every caster.
        glm::mat4 proj = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, far_d);
        proj[1][1] *= -1.0f;  // Vulkan Y-flip
        glm::mat4 reverse_z(1.0f);
        reverse_z[2][2] = -1.0f;
        reverse_z[3][2] = 1.0f;
        proj = reverse_z * proj;

        frame_.cascade_view_proj_[c] = proj * view;
        // Conservative world-space bounding sphere of this cascade for the draw-level shadow cull.
        frame_.cascade_center_[c] = center;
        frame_.cascade_cull_radius_[c] = radius + far_d;
        last_split = split;
    }
}

SkyParams sky_params(const scene_frame& f)
{
    return SkyParams{
        .view_proj = f.view_proj,
        .camera_pos = f.camera_pos,
        .sun_dir = f.env.sun_dir,
        .sky_zenith = f.env.sky_zenith,
        .sky_ground = f.env.sky_ground,
        .sun_color = f.env.sun_color,
        .sun_intensity = f.env.sun_intensity,
        .furnace = f.env.furnace,
    };
}

FroxelParams froxel_params(const scene_frame& f)
{
    return FroxelParams{
        .view = f.view,
        .inv_proj = glm::inverse(f.view_proj * glm::inverse(f.view)),
        .near_plane = f.near_plane,
        .far_plane = f.far_plane,
        .light_count = f.env.lights_enabled ? static_cast<uint32_t>(f.lights.size()) : 0u,
    };
}

IblLighting ibl_lighting(const scene_frame& f)
{
    return IblLighting{
        .sun_dir = f.env.sun_dir,
        .sky_zenith = f.env.sky_zenith,
        .sky_ground = f.env.sky_ground,
        .sun_color = f.env.sun_color,
        .sun_intensity = f.env.sun_intensity,
        .furnace = f.env.furnace,
    };
}

}  // namespace string::render
