#pragma once

#include <cstdint>
#include <string>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace sandbox
{

// A synthetic world-space anchor: a point the UI author can project to screen for a world-anchored
// nameplate / ping marker. Kept sandbox-side so nameplate projection works IDENTICALLY whether the
// full GeometryPass camera is present (demo scene) or not (ui-dev scene) — the UI never reaches into
// the geometry pass; it reads this shared view state instead.
struct UiAnchor
{
    glm::vec3 world{ 0.0f };
    std::string name;      // player/mob name (may hold arbitrary Unicode)
    float health = 1.0f;   // 0..1
    float cast = -1.0f;    // 0..1 cast progress, <0 = not casting
    bool hostile = false;
};

// The projection state + synthetic anchors the UI author reads each frame. Shared (shared_ptr) as the
// seam between whatever populates the view (a background pass in the ui-dev scene) and the UI author.
// One instance per plan; updated before UIPass::update reads it (author runs inside UIPass::update).
struct UiScene
{
    glm::mat4 view_proj{ 1.0f };
    glm::vec3 camera_pos{ 0.0f };
    glm::vec2 screen{ 1.0f };  // px; set by the driver each frame
    std::vector<UiAnchor> anchors;

    // Project a world point to screen pixels (top-left origin). Returns false if behind the camera or
    // off screen; `out_depth` is clip-space w (view distance proxy) for distance scaling/fade.
    bool project(const glm::vec3& world, glm::vec2& out_px, float& out_depth) const
    {
        const glm::vec4 clip = view_proj * glm::vec4(world, 1.0f);
        if (clip.w <= 0.0001f)
            return false;
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        out_px.x = (ndc.x * 0.5f + 0.5f) * screen.x;
        out_px.y = (ndc.y * 0.5f + 0.5f) * screen.y;  // Vulkan y-down: NDC +y is screen +y
        out_depth = clip.w;
        return ndc.x >= -1.3f && ndc.x <= 1.3f && ndc.y >= -1.3f && ndc.y <= 1.3f && ndc.z >= 0.0f;
    }
};

// A tiny orbiting perspective camera + synthetic anchor field for the ui-dev scene. Drives a UiScene
// each frame so nameplate/ping projection has something to track without any real geometry. Also used
// by the demo scene author (over Sponza) so the nameplate stress screen is identical in both.
class UiSceneDriver
{
public:
    explicit UiSceneDriver(std::uint32_t anchor_count = 0)
    {
        rebuild_anchors(anchor_count);
    }

    // Regenerate `count` synthetic anchors in a slab of world space, deterministic (seeded), with a
    // spread of Unicode names to exercise the dynamic glyph atlas.
    void rebuild_anchors(std::uint32_t count)
    {
        anchors_.clear();
        anchors_.reserve(count);
        static const char* names[] = {
            "Aeloria", "Дракон", "力士", "Bjørn", "Zoë", "Naïve", "Мурка", "花子",
            "Þórr", "Ελληνας", "Cœur", "Ünal", "Łukasz", "José", "Владимир", "王小明",
        };
        std::uint32_t seed = 0x9e3779b9u;
        auto rnd = [&]() {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            return (seed & 0xFFFFFF) / float(0xFFFFFF);
        };
        for (std::uint32_t i = 0; i < count; ++i)
        {
            UiAnchor a;
            const float ang = rnd() * 6.2831853f;
            const float rad = 4.0f + rnd() * 18.0f;
            a.world = glm::vec3(std::cos(ang) * rad, 0.5f + rnd() * 3.0f, std::sin(ang) * rad);
            a.name = std::string(names[i % (sizeof(names) / sizeof(names[0]))]);
            if (count > 1)
                a.name += " " + std::to_string(i);
            a.health = 0.15f + rnd() * 0.85f;
            a.cast = (rnd() < 0.25f) ? rnd() : -1.0f;
            a.hostile = rnd() < 0.5f;
            anchors_.push_back(std::move(a));
        }
    }

    // Advance the orbit and write the projection + anchors into `scene` for this frame.
    void update(float delta_time, std::uint32_t screen_w, std::uint32_t screen_h, UiScene& scene)
    {
        orbit_ += delta_time * 0.15f;
        const float dist = 22.0f;
        const glm::vec3 eye(std::cos(orbit_) * dist, 8.0f, std::sin(orbit_) * dist);
        const glm::vec3 target(0.0f, 2.0f, 0.0f);
        const glm::mat4 view = glm::lookAt(eye, target, glm::vec3(0, 1, 0));
        const float aspect = screen_h > 0 ? float(screen_w) / float(screen_h) : 1.7f;
        glm::mat4 proj = glm::perspective(glm::radians(55.0f), aspect, 0.1f, 200.0f);
        proj[1][1] *= -1.0f;  // Vulkan clip: flip Y so +Y world maps to +Y down-screen consistently
        scene.view_proj = proj * view;
        scene.camera_pos = eye;
        scene.screen = glm::vec2(float(screen_w), float(screen_h));
        scene.anchors = anchors_;
    }

    std::size_t anchor_count() const { return anchors_.size(); }

private:
    std::vector<UiAnchor> anchors_;
    float orbit_ = 0.0f;
};

}  // namespace sandbox
