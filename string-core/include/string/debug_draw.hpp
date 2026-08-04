#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

// Immediate-mode debug drawing (brief 06). Free-function API callable from ANY system — a pass, a
// gameplay tick, an inspector — with no GPU objects in scope. Calls accumulate into a process-global
// per-frame vertex ring; a renderer-side line pass consumes the ring after the scene, before UI.
//
//     string::debug::line({0,0,0}, {1,0,0}, {255,0,0,255});   // depth-tested red line
//     string::debug::aabb(min, max, {0,255,0,255});           // wireframe box
//     string::debug::sphere(center, radius, {0,128,255,255}); // 3 great-circle rings
//     string::debug::axes(transform, 0.5f);                   // RGB gizmo
//     string::debug::text3d({x,y,z}, "label", {255,255,255,255});  // billboard label (UI pass)
//
// Every primitive has a depth-tested (default) and an overlay (`_overlay`, always-on-top) variant.
// Colours are 8-bit RGBA. The context is cleared once per frame by the renderer via begin_frame().
// Thread-safe (a mutex guards the vectors) so gameplay/worker threads can draw; contention is a
// non-issue at debug-draw volumes.
namespace string::debug
{

struct Color
{
    uint8_t r = 255, g = 255, b = 255, a = 255;
};

// One line vertex: world position + packed RGBA. Matches the debug_line.slang vertex layout.
struct LineVertex
{
    glm::vec3 pos;
    uint32_t rgba;  // r | g<<8 | b<<16 | a<<24
};

// A world-anchored text label, projected + drawn by the UI pass (brief 06 text3d).
struct TextLabel
{
    glm::vec3 pos;
    Color color;
    std::string text;
};

// The per-frame accumulation buffers. One process-global instance (see context()).
class DebugDrawContext
{
public:
    static DebugDrawContext& instance();

    void clear();  // called once per frame by the renderer before systems draw

    void add_line(const glm::vec3& a, const glm::vec3& b, Color c, bool overlay);
    void add_text(const glm::vec3& pos, std::string text, Color c);

    // Consumed by the line pass. Spans are valid until the next clear().
    std::span<const LineVertex> depth_vertices() const { return depth_verts_; }
    std::span<const LineVertex> overlay_vertices() const { return overlay_verts_; }
    std::span<const TextLabel> labels() const { return labels_; }

    std::mutex& mutex() { return mutex_; }

private:
    DebugDrawContext() = default;
    mutable std::mutex mutex_;
    std::vector<LineVertex> depth_verts_;
    std::vector<LineVertex> overlay_verts_;
    std::vector<TextLabel> labels_;
};

inline DebugDrawContext& context() { return DebugDrawContext::instance(); }

// --- Primitives (depth-tested) ----------------------------------------------
void line(const glm::vec3& a, const glm::vec3& b, Color c = {});
void aabb(const glm::vec3& min, const glm::vec3& max, Color c = {});
void sphere(const glm::vec3& center, float radius, Color c = {}, int segments = 24);
void axes(const glm::mat4& transform, float length = 1.0f);
void text3d(const glm::vec3& pos, std::string text, Color c = {});

// --- Overlay variants (always on top, no depth test) ------------------------
void line_overlay(const glm::vec3& a, const glm::vec3& b, Color c = {});
void aabb_overlay(const glm::vec3& min, const glm::vec3& max, Color c = {});
void sphere_overlay(const glm::vec3& center, float radius, Color c = {}, int segments = 24);

}  // namespace string::debug
