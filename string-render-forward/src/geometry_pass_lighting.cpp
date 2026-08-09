// shadow cascade fit, HiZ pyramid, GTAO, local-light stress — split out of geometry_pass.cpp (brief 11 modularization). These remain geometry_pass
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
using namespace string;

// Brief 20: this used to allocate the depth + pyramid rings, create per-mip views, claim bindless
// slots and build a sampler. All of that is the graph's now — the pyramid is an application-declared
// transient and every slot comes from ctx.slot(pyramid.mip(m)) while the pass records. What is left
// is the SHAPE, which the technique still needs to size its dispatches.
void geometry_pass::ensure_hiz(uint16_t current_frame)
{
    (void)current_frame;
    if (screen_size.width == 0 || screen_size.height == 0) return;
    const VkExtent2D base = hiz_extent(screen_size);
    if (base.width == hiz_screen_w_ && base.height == hiz_screen_h_) return;
    hiz_screen_w_ = base.width;
    hiz_screen_h_ = base.height;

    // The pyramid IMAGE follows the viewport by declaration now (viewport_fit::half_pow2 + all_mips,
    // brief 21 step 2), so the dispatch shape is just the same relationship computed here — no clamp,
    // because there is no longer a fixed declared extent to outgrow.
    const uint32_t mips = hiz_mip_count(screen_size);
    hiz_.assign(frames_in_flight_, HizPyramid{ mips, glm::uvec2(base.width, base.height) });
    STRING_LOG_INFO("[hiz] pyramid {}x{}, {} mips", base.width, base.height, mips);

    // The resolved-depth contents every slot held are gone with the resize, so GTAO's reprojection
    // input is invalid until each slot is rendered again. SIZING is load-bearing: the brief-20
    // rewrite deleted the hiz ring allocation that used to size this vector, leaving it EMPTY —
    // GTAO's `prev < depth_history_.size()` readiness check then never passed, GTAO silently never
    // ran, and its consumers sampled the 0.5 neutral fallback whose decoded bent normal is the
    // zero vector — the giant pose-dependent black regions that presented as broken shadows.
    depth_history_.assign(frames_in_flight_, DepthHistorySlot{});
}

void geometry_pass::compute_cascades()
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

void geometry_pass::build_light_stress_scene(const glm::vec3& aabb_min, const glm::vec3& aabb_max)
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

void geometry_pass::animate_lights(float delta_time)
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


// The HiZ pyramid's shape, as a pure function of the viewport. Both the application (declaring the
// transient) and geometry_pass::declare (authoring one pass per mip) must agree on this, and
// authored-once means they compute it ONCE from the initial viewport rather than per frame. Mip 0 is
// half of the next power of two, which keeps the reduction exact at every level.
VkExtent2D geometry_pass::hiz_extent(VkExtent2D viewport)
{
    const auto next_pow2 = [](uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; };
    return { std::max(1u, next_pow2(viewport.width) / 2), std::max(1u, next_pow2(viewport.height) / 2) };
}

uint32_t geometry_pass::hiz_mip_count(VkExtent2D viewport)
{
    const VkExtent2D base = hiz_extent(viewport);
    return static_cast<uint32_t>(std::floor(std::log2(std::max(base.width, base.height)))) + 1;
}

// The per-frame parameter blocks the sky, froxel and IBL components latch. Every field is already
// scene state; this is assembly, not policy.
SkyParams GeometryScene::sky_params() const
{
    return SkyParams{
        .view_proj = camera_.view_proj(),
        .camera_pos = camera_.position(),
        .sun_dir = sun_dir_,
        .sky_zenith = sky_zenith_,
        .sky_ground = sky_ground_,
        .sun_color = sun_color_,
        .sun_intensity = sun_intensity_,
        .furnace = furnace_,
    };
}

FroxelParams GeometryScene::froxel_params() const
{
    return FroxelParams{
        .view = camera_.view(),
        .inv_proj = glm::inverse(camera_.view_proj() * glm::inverse(camera_.view())),
        .near_plane = camera_.near_plane(),
        .far_plane = camera_.far_plane(),
        .light_count = lights_enabled_ ? static_cast<uint32_t>(lights_.size()) : 0u,
    };
}

IblLighting GeometryScene::ibl_lighting() const
{
    return IblLighting{
        .sun_dir = sun_dir_,
        .sky_zenith = sky_zenith_,
        .sky_ground = sky_ground_,
        .sun_color = sun_color_,
        .sun_intensity = sun_intensity_,
        .furnace = furnace_,
    };
}

std::size_t geometry_pass::gi_capture_entries() const
{
    return probe_gi_component::capture_table(meshlet_model_).size();
}

}  // namespace string::render
