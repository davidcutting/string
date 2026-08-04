// shadow cascade fit, HiZ pyramid, GTAO, local-light stress — split out of geometry_pass.cpp (brief 11 modularization). These remain GeometryPass
// member functions (cohesive translation-unit split; the geometry core stays one class, its true
// graph-pass decoupling is Phase 2). State lives in geometry_pass.hpp.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <string/core/logger.hpp>
#include <string/gpu/command_recorder.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/gpu/shader_compiler.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <string/vulkan/passes/composite_pass.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <string/render/geometry_pass.hpp>
#include <string/render/render_cvars.hpp>

namespace string::render
{
using namespace String;

void GeometryPass::ensure_hiz(uint16_t current_frame)
{
    if (screen_size.width == 0 || screen_size.height == 0) return;
    // Conservative power-of-two base covering the screen (so a min-reduce never misses a texel).
    auto next_pow2 = [](uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; };
    const uint32_t base_w = next_pow2(screen_size.width) / 2;   // half-res mip0 is plenty for HiZ
    const uint32_t base_h = next_pow2(screen_size.height) / 2;
    if (base_w == hiz_screen_w_ && base_h == hiz_screen_h_ && hiz_[current_frame].image != 0) return;
    hiz_screen_w_ = base_w;
    hiz_screen_h_ = base_h;

    const uint32_t mips = static_cast<uint32_t>(std::floor(std::log2(std::max(base_w, base_h)))) + 1;

    // Retire the old per-frame VIEWS + bindless SLOTS first (they reference the current physicals,
    // which recreate_per_frame_image is about to free).
    for (uint32_t f = 0; f < frames_in_flight_; ++f)
    {
        HizPyramid& hz = hiz_[f];
        for (uint32_t s : hz.mip_storage_slots) descriptor_table_.unbind_storage_view(s);
        for (VkImageView v : hz.mip_views) vkDestroyImageView(device_.get_device(), v, nullptr);
        if (hz.image != 0) descriptor_table_.unbind(hz.image, ::string::gpu::descriptor_type::TEXTURE);
        if (hz.depth != 0) descriptor_table_.unbind(hz.depth, ::string::gpu::descriptor_type::TEXTURE);
    }

    // Brief 16 M7 (#1): (re)allocate the depth + pyramid image RINGS via the registry (allocation
    // authority + owns/frees them). First call creates; a resize recreates in place.
    const ::string::gpu::image_info depth_info{
        .extent = { screen_size.width, screen_size.height, 1 },
        .format = VK_FORMAT_D32_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_DEPTH_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    };
    const ::string::gpu::image_info pyramid_info{
        .extent = { base_w, base_h, 1 },
        .format = VK_FORMAT_R32_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
        .mip_levels = mips,
    };
    if (hiz_depth_ring_.valid()) resources->recreate_per_frame_image(hiz_depth_ring_, depth_info);
    else                         hiz_depth_ring_ = resources->create_per_frame_image(depth_info, frames_in_flight_);
    if (hiz_pyramid_ring_.valid()) resources->recreate_per_frame_image(hiz_pyramid_ring_, pyramid_info);
    else                           hiz_pyramid_ring_ = resources->create_per_frame_image(pyramid_info, frames_in_flight_);

    for (uint32_t f = 0; f < frames_in_flight_; ++f)
    {
        HizPyramid& hz = hiz_[f];
        hz = HizPyramid{};
        hz.mips = mips;
        hz.size = glm::uvec2(base_w, base_h);

        // Single-sample D32 depth (registry-owned) for the camera prepass the pyramid reduces from.
        hz.depth = resources->physical(hiz_depth_ring_, f);
        descriptor_table_.bind(hz.depth, ::string::gpu::descriptor_type::TEXTURE);
        hz.depth_slot = descriptor_table_.get_binding_slot(hz.depth, ::string::gpu::descriptor_type::TEXTURE);
        descriptor_table_.update_texture(hz.depth_slot, allocator_.get_image(hz.depth).view, hiz_sampler_);
        hz.image = resources->physical(hiz_pyramid_ring_, f);
        const ::string::gpu::allocated_image& img = allocator_.get_image(hz.image);

        // Whole-chain sampled slot (for SampleLevel in the task shader).
        descriptor_table_.bind(hz.image, ::string::gpu::descriptor_type::TEXTURE);
        hz.sample_slot = descriptor_table_.get_binding_slot(hz.image, ::string::gpu::descriptor_type::TEXTURE);
        descriptor_table_.update_texture(hz.sample_slot, img.view, hiz_sampler_);

        // Per-mip storage views + slots (downsample writes mip N+1 while sampling mip N).
        for (uint32_t m = 0; m < mips; ++m)
        {
            VkImageViewCreateInfo vi = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = img.image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = VK_FORMAT_R32_SFLOAT,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1 },
            };
            VkImageView view = VK_NULL_HANDLE;
            vkCreateImageView(device_.get_device(), &vi, nullptr, &view);
            hz.mip_views.push_back(view);
            hz.mip_storage_slots.push_back(descriptor_table_.bind_storage_view(view));
        }
    }
    STRING_LOG_INFO("[hiz] pyramid {}x{}, {} mips (x{} frames)", base_w, base_h, mips, frames_in_flight_);
    // Brief 09: the hz.depth images were just recreated — every slot's resolved-depth content
    // (GTAO's input) is gone.
    std::fill(hz_depth_valid_.begin(), hz_depth_valid_.end(), uint8_t(0));
}

void GeometryPass::ensure_gtao()
{
    if (screen_size.width == 0 || screen_size.height == 0) return;
    const glm::uvec2 size((screen_size.width + 1) / 2, (screen_size.height + 1) / 2);
    if (gtao_raw_ != 0 && size == gtao_size_) return;
    gtao_size_ = size;

    // Retire the old bindless slots (the registry frees the physicals via recreate below).
    if (gtao_raw_storage_slot_ != UINT32_MAX)
        descriptor_table_.unbind_storage_view(gtao_raw_storage_slot_);
    for (uint32_t s : gtao_final_storage_slots_) descriptor_table_.unbind_storage_view(s);
    if (gtao_raw_ != 0) descriptor_table_.unbind(gtao_raw_, ::string::gpu::descriptor_type::TEXTURE);
    for (::string::gpu::resource_id id : gtao_final_)
        if (id != 0) descriptor_table_.unbind(id, ::string::gpu::descriptor_type::TEXTURE);
    gtao_final_storage_slots_.clear();
    gtao_final_sampled_slots_.clear();
    gtao_final_.assign(frames_in_flight_, 0);
    gtao_final_ready_.assign(frames_in_flight_, 0);
    gtao_raw_initialized_ = false;

    // Brief 16 M7 (#1): (re)allocate the raw (shared, 1-deep) + final (per-frame) image rings via
    // the registry. RGBA16F, not RGBA8: 8-bit visibility quantizes into wide soft bands on smooth
    // slowly-curving receivers (the Sponza vaults). Half-res 16F is ~4 B/px extra; the bent normal
    // rides along at the higher precision for free.
    const ::string::gpu::image_info gtao_info{
        .extent = { size.x, size.y, 1 },
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    };
    if (gtao_raw_ring_.valid()) resources->recreate_per_frame_image(gtao_raw_ring_, gtao_info);
    else                        gtao_raw_ring_ = resources->create_per_frame_image(gtao_info, 1);
    if (gtao_final_ring_.valid()) resources->recreate_per_frame_image(gtao_final_ring_, gtao_info);
    else                          gtao_final_ring_ = resources->create_per_frame_image(gtao_info, frames_in_flight_);

    const auto bind_target = [&](::string::gpu::resource_id id, uint32_t& sampled_slot, uint32_t& storage_slot) {
        const ::string::gpu::allocated_image& img = allocator_.get_image(id);
        descriptor_table_.bind(id, ::string::gpu::descriptor_type::TEXTURE);
        sampled_slot = descriptor_table_.get_binding_slot(id, ::string::gpu::descriptor_type::TEXTURE);
        descriptor_table_.update_texture(sampled_slot, img.view, gtao_sampler_);
        storage_slot = descriptor_table_.bind_storage_view(img.view);
    };
    gtao_raw_ = resources->physical(gtao_raw_ring_, 0);
    bind_target(gtao_raw_, gtao_raw_sampled_slot_, gtao_raw_storage_slot_);
    gtao_final_sampled_slots_.resize(frames_in_flight_);
    gtao_final_storage_slots_.resize(frames_in_flight_);
    for (uint32_t f = 0; f < frames_in_flight_; ++f)
    {
        gtao_final_[f] = resources->physical(gtao_final_ring_, f);
        bind_target(gtao_final_[f], gtao_final_sampled_slots_[f], gtao_final_storage_slots_[f]);
    }
    STRING_LOG_INFO("[gtao] half-res targets {}x{} (raw + {} final slots)", size.x, size.y,
                    frames_in_flight_);
}

void GeometryPass::record_gtao(::string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    const uint16_t prev = (current_frame + frames_in_flight_ - 1) % frames_in_flight_;
    if (prev >= hiz_.size() || hiz_[prev].depth == 0 || gtao_raw_ == 0) return;
    // Brief 16: GTAO is dense barrier + vku::transition_image + dispatch raw Vulkan — use the recorder's
    // vk() escape for the transition/barrier tail; the clean dispatches below go through verbs.
    VkCommandBuffer cb = recorder.vk();
    const HizPyramid& hz = hiz_[prev];
    const ::string::gpu::allocated_image& depth = allocator_.get_image(hz.depth);
    const ::string::gpu::allocated_image& raw = allocator_.get_image(gtao_raw_);
    const ::string::gpu::allocated_image& fin = allocator_.get_image(gtao_final_[current_frame]);

    STRING_PROFILE_GPU_ZONE(gpu_ctx(), cb, "gtao")

    // Prev-frame resolved depth: DEPTH_ATTACHMENT (where record_between parked it) -> sampled.
    // Left in SHADER_READ_ONLY afterwards — the renderer's next resolve of this slot re-discards
    // from UNDEFINED with a src scope that already names COMPUTE sampled reads.
    vku::transition_image(cb, {
        .image = depth.image,
        .old_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                   | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .src_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
    });
    if (!gtao_raw_initialized_)
    {
        gtao_raw_initialized_ = true;
        vku::transition_image(cb, {
            .image = raw.image,
            .old_layout = VK_IMAGE_LAYOUT_UNDEFINED,
            .new_layout = VK_IMAGE_LAYOUT_GENERAL,
            .src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, .src_access = 0,
            .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dst_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        });
    }
    else
    {
        vku::transition_image(cb, {
            .image = raw.image,
            .old_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .new_layout = VK_IMAGE_LAYOUT_GENERAL,
            .src_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .src_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dst_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        });
    }
    vku::transition_image(cb, {
        .image = fin.image,
        .old_layout = gtao_final_ready_[current_frame] ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                       : VK_IMAGE_LAYOUT_UNDEFINED,
        .new_layout = VK_IMAGE_LAYOUT_GENERAL,
        .src_stage = gtao_final_ready_[current_frame]
            ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .src_access = gtao_final_ready_[current_frame]
            ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VkAccessFlags2(0),
        .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dst_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
    });

    const glm::mat4& proj = gtao_slot_proj_[prev];
    GtaoPush push{};
    push.view = gtao_slot_view_[prev];
    push.inv_proj = glm::inverse(proj);
    push.depth_slot = hz.depth_slot;
    push.dst_slot = gtao_raw_storage_slot_;
    push.dst_size = gtao_size_;
    push.depth_size = glm::uvec2(screen_size.width, screen_size.height);
    push.radius = std::max(cv_gtao_radius().get(), 0.01f);
    push.proj00 = std::abs(proj[0][0]);
    push.proj11 = std::abs(proj[1][1]);

    VkDescriptorSet set = descriptor_table_.get_set();
    const auto dispatch = [&](::string::gpu::shader_program* prog, const GtaoPush& p_push) {
        const ::string::gpu::pipeline& p = prog->current();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline_layout,
                                0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(GtaoPush), &p_push);
        vkCmdDispatch(cb, (gtao_size_.x + 7) / 8, (gtao_size_.y + 7) / 8, 1);
    };
    dispatch(gtao_program_, push);

    // raw writes -> denoise sampled reads.
    vku::transition_image(cb, {
        .image = raw.image,
        .old_layout = VK_IMAGE_LAYOUT_GENERAL,
        .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .src_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
    });
    GtaoPush denoise = push;
    denoise.src_slot = gtao_raw_sampled_slot_;
    denoise.dst_slot = gtao_final_storage_slots_[current_frame];
    dispatch(gtao_denoise_program_, denoise);

    // Final -> sampled for this frame's lit fragments.
    vku::transition_image(cb, {
        .image = fin.image,
        .old_layout = VK_IMAGE_LAYOUT_GENERAL,
        .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .src_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
    });
    gtao_final_ready_[current_frame] = 1;
}

void GeometryPass::compute_cascades()
{
    // Practical split scheme (Zhang): blend a logarithmic and a uniform split by cascade_split_lambda,
    // over [near, shadow_depth_range]. Each cascade's ortho box is fit to that view-space depth slice's
    // frustum corners, then STABILIZED: the box is sized to a sphere (rotation-invariant so the sun
    // moving doesn't resize it) and its origin snapped to whole shadow texels (so neither the sun nor
    // the camera moving shimmers the shadow edges).
    const uint32_t count = std::min(settings_.cascade_count, kMaxCascades);
    const float near_clip = camera_.near_plane();
    const float far_clip = std::min(settings_.shadow_depth_range, camera_.far_plane());
    const float range = far_clip - near_clip;

    const glm::mat4 inv_cam = glm::inverse(camera_.view_proj());
    const glm::vec3 L = glm::normalize(sun_dir_);   // direction TO the sun

    float last_split = near_clip;
    for (uint32_t c = 0; c < count; ++c)
    {
        const float p = static_cast<float>(c + 1) / static_cast<float>(count);
        const float log_split = near_clip * std::pow(far_clip / near_clip, p);
        const float uniform_split = near_clip + range * p;
        const float split = settings_.cascade_split_lambda * log_split
                          + (1.0f - settings_.cascade_split_lambda) * uniform_split;
        cascade_split_[c] = split;

        // The 8 corners of this cascade's sub-frustum, mapped from NDC (reverse-Z: near=1, far=0) into
        // world via the inverse camera view-projection, then re-scaled so its near/far match this
        // slice's [last_split, split] view-space depths.
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
        // Split the near/far corner pairs (indices z=0 far, z=1 near in reverse-Z above) so this
        // cascade covers [last_split, split] of the camera's linear depth. March along the frustum
        // edge FROM the near corner TOWARD the far corner by (view_depth - near_clip)/range: at
        // last_split we get this cascade's near edge, at split its far edge. (The previous code based
        // the lerp on the FAR corner with an inverted parameter, so last_split=near_clip returned the
        // far corner — planting every cascade kilometres out at the camera far plane, ~2 m/texel, so
        // the scene got ~15 texels of shadow and nothing resolved.)
        // The frustum edge spans the CAMERA's full depth (near .. camera far plane), so the lerp
        // parameter that maps a view-space depth onto that edge must divide by the camera's far
        // distance, NOT shadow_depth_range. (`range` above is only for the split SCHEME over
        // [near, shadow_depth_range]; using it here left cascades ~camera_far/shadow_range x too big.)
        const float cam_range = camera_.far_plane() - near_clip;
        for (int i = 0; i < 4; ++i)
        {
            const glm::vec3 far_c = corners[i * 2 + 0];   // z=0 -> camera far plane
            const glm::vec3 near_c = corners[i * 2 + 1];  // z=1 -> camera near plane
            const glm::vec3 edge = far_c - near_c;        // near -> far along this frustum edge
            corners[i * 2 + 0] = near_c + edge * ((split - near_clip) / cam_range);       // cascade far edge
            corners[i * 2 + 1] = near_c + edge * ((last_split - near_clip) / cam_range);  // cascade near edge
        }

        glm::vec3 center(0.0f);
        for (const glm::vec3& corner : corners) center += corner;
        center /= 8.0f;

        float radius = 0.0f;
        for (const glm::vec3& corner : corners) radius = std::max(radius, glm::length(corner - center));
        radius = std::ceil(radius * 16.0f) / 16.0f;   // quantize the radius so it doesn't jitter

        const float texel = (2.0f * radius) / static_cast<float>(settings_.shadow_resolution);
        cascade_world_texel_[c] = texel;

        // Depth range: bracket the SCENE AABB along the light axis (a BOUNDED extent), NOT the cascade
        // radius. Pulling the eye back by `radius` blew the far cascades' depth range (radius >> the
        // 30 m scene, so cascade 2 got a ~1.5 km-deep box) — depth precision collapsed and distant /
        // grazing floor lost its shadows on rotation. Projecting the scene AABB onto L gives the true
        // caster..receiver span; the eye sits just behind the nearest-to-sun caster, far reaches the
        // farthest receiver. XY still uses `radius`; only near/far come from the scene.
        float min_proj = std::numeric_limits<float>::max();
        float max_proj = std::numeric_limits<float>::lowest();
        for (int ci = 0; ci < 8; ++ci)
        {
            const glm::vec3 corner((ci & 1) ? scene_aabb_max_.x : scene_aabb_min_.x,
                                   (ci & 2) ? scene_aabb_max_.y : scene_aabb_min_.y,
                                   (ci & 4) ? scene_aabb_max_.z : scene_aabb_min_.z);
            const float d = glm::dot(corner - center, L);   // signed distance along L (toward sun = +)
            min_proj = std::min(min_proj, d);
            max_proj = std::max(max_proj, d);
        }
        const float margin = 1.0f;
        const float eye_back = max_proj + margin;                   // eye just behind the sun-most caster
        const float far_d = (max_proj - min_proj) + 2.0f * margin;  // reach the farthest receiver
        const glm::vec3 up = std::abs(L.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::vec3 eye = center + L * eye_back;
        glm::mat4 view = glm::lookAt(eye, center, up);

        // Stabilize: snap the cascade center to whole shadow texels along the light's lateral axes.
        // CRITICAL: measuring in `view` space is a NO-OP -- lookAt(eye, center, up) puts `center` at
        // the light-space origin every frame (x=y=0), so std::floor(0) snaps nothing and the map slid
        // sub-texel each frame. With NEAREST sampling that flips every silhouette texel occluder<->empty
        // per frame -> the magenta sparkle, worst under motion. Measure in a FIXED light BASIS (rotation
        // only, no per-frame translation) so the lateral position actually quantizes to the texel grid.
        const glm::mat3 light_basis = glm::mat3(glm::lookAt(glm::vec3(0.0f), -L, up));  // rotation only
        glm::vec3 center_ls = light_basis * center;
        center_ls.x = std::floor(center_ls.x / texel) * texel;
        center_ls.y = std::floor(center_ls.y / texel) * texel;
        const glm::vec3 snapped_center = glm::transpose(light_basis) * center_ls;  // back to world
        const glm::vec3 snapped_eye = snapped_center + L * eye_back;
        view = glm::lookAt(snapped_eye, snapped_center, up);

        // Ortho box: [-radius, radius] in x/y; near=0 (at the eye), far = scene depth span along L.
        // Use orthoRH_ZO explicitly (NOT glm::ortho): GLM_FORCE_DEPTH_ZERO_TO_ONE is not reliably in
        // effect in THIS translation unit (engine headers above pull glm in default [-1,1] mode before
        // the macro lands), so plain glm::ortho produced an OpenGL [-1,1] box. The reverse_z below
        // assumes [0,1], so the sunward half mapped to z_ndc>1 and got hard-clipped -- slicing the top
        // (roof/upper walls) off every caster, which is why tall geometry cast no shadow.
        glm::mat4 proj = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, far_d);
        proj[1][1] *= -1.0f;  // Vulkan Y-flip
        glm::mat4 reverse_z(1.0f);
        reverse_z[2][2] = -1.0f;
        reverse_z[3][2] = 1.0f;
        proj = reverse_z * proj;

        cascade_view_proj_[c] = proj * view;
        // Brief 04 M4: conservative world-space bounding sphere of this cascade for the draw-level
        // shadow cull. XY is bounded by `radius`; the ortho depth now spans the scene AABB along L
        // (far_d), so inflate the cull radius to cover the XY disc + full depth slab. Conservative
        // (never under-covers) -> off-cascade casters are safely kept.
        cascade_center_[c] = center;
        cascade_cull_radius_[c] = radius + far_d;
        last_split = split;
    }
}

void GeometryPass::build_light_stress_scene(const glm::vec3& aabb_min, const glm::vec3& aabb_max)
{
    // Hundreds of colored point + spot lights orbiting over the model — the brief's light stress test
    // bed. Deterministic pseudo-random placement so runs are comparable. Radii/intensities sized to
    // the scene so froxel binning is meaningfully exercised without washing everything out.
    constexpr uint32_t kStressLights = 384;
    const glm::vec3 extent = aabb_max - aabb_min;
    const glm::vec3 center = (aabb_min + aabb_max) * 0.5f;
    const float scene_scale = glm::length(extent);
    const float light_range = scene_scale * 0.06f;

    uint32_t seed = 0x1234567u;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / static_cast<float>(0xFFFFFF); };

    lights_.clear();
    light_anim_.clear();
    lights_.reserve(kStressLights);
    light_anim_.reserve(kStressLights);
    for (uint32_t i = 0; i < kStressLights; ++i)
    {
        const bool spot = (i % 4) == 0;
        // A warm/saturated palette so overlapping lights read distinctly.
        const glm::vec3 color = glm::vec3(0.4f + 0.6f * rnd(), 0.4f + 0.6f * rnd(), 0.4f + 0.6f * rnd());
        LightAnim a;
        a.center = center + glm::vec3((rnd() - 0.5f) * extent.x, aabb_min.y + extent.y * (0.15f + 0.5f * rnd()),
                                      (rnd() - 0.5f) * extent.z);
        a.radius = extent.x * (0.05f + 0.25f * rnd());
        a.speed = (0.3f + 1.2f * rnd()) * (rnd() > 0.5f ? 1.0f : -1.0f);
        a.phase = rnd() * 6.2831853f;
        a.height = extent.y * 0.15f * rnd();
        light_anim_.push_back(a);

        GpuLight L{};
        L.position_radius = glm::vec4(a.center, light_range);
        // Brief 07 M4 units: luminous intensity in kilocandela (illuminance = I/d^2 in klx).
        // Stress lights are deliberately floodlight-class so they still read against daylight.
        L.color_intensity = glm::vec4(color, spot ? 300.0f : 150.0f);
        const glm::vec3 dir = glm::normalize(glm::vec3(rnd() - 0.5f, -1.0f, rnd() - 0.5f));
        L.direction_type = glm::vec4(dir, spot ? 1.0f : 0.0f);
        L.cone = glm::vec4(std::cos(glm::radians(18.0f)), std::cos(glm::radians(30.0f)), 0.0f, 0.0f);
        lights_.push_back(L);
    }
    STRING_LOG_INFO("[light] stress scene: {} lights ({} spot), range {:.2f}",
                    lights_.size(), kStressLights / 4, light_range);
}

void GeometryPass::animate_lights(float delta_time)
{
    static float t = 0.0f;
    t += delta_time;
    for (std::size_t i = 0; i < lights_.size(); ++i)
    {
        const LightAnim& a = light_anim_[i];
        const float ang = a.phase + t * a.speed;
        lights_[i].position_radius.x = a.center.x + std::cos(ang) * a.radius;
        lights_[i].position_radius.z = a.center.z + std::sin(ang) * a.radius;
        lights_[i].position_radius.y = a.center.y + std::sin(ang * 1.7f) * a.height;
    }
}

}  // namespace string::render
