#include <string/debug_draw.hpp>

#include <cmath>

namespace string::debug
{

namespace
{
uint32_t pack(Color c)
{
    return uint32_t(c.r) | (uint32_t(c.g) << 8) | (uint32_t(c.b) << 16) | (uint32_t(c.a) << 24);
}
}  // namespace

DebugDrawContext& DebugDrawContext::instance()
{
    static DebugDrawContext ctx;
    return ctx;
}

void DebugDrawContext::clear()
{
    std::lock_guard lock(mutex_);
    depth_verts_.clear();
    overlay_verts_.clear();
    labels_.clear();
}

void DebugDrawContext::add_line(const glm::vec3& a, const glm::vec3& b, Color c, bool overlay)
{
    std::lock_guard lock(mutex_);
    std::vector<LineVertex>& dst = overlay ? overlay_verts_ : depth_verts_;
    dst.push_back(LineVertex{ a, pack(c) });
    dst.push_back(LineVertex{ b, pack(c) });
}

void DebugDrawContext::add_text(const glm::vec3& pos, std::string text, Color c)
{
    std::lock_guard lock(mutex_);
    labels_.push_back(TextLabel{ pos, c, std::move(text) });
}

// --- primitive builders -----------------------------------------------------

static void emit_aabb(const glm::vec3& mn, const glm::vec3& mx, Color c, bool overlay)
{
    const glm::vec3 v[8] = {
        { mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z }, { mx.x, mx.y, mn.z }, { mn.x, mx.y, mn.z },
        { mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z }, { mx.x, mx.y, mx.z }, { mn.x, mx.y, mx.z },
    };
    static const int e[12][2] = {
        { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },  // bottom
        { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },  // top
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },  // verticals
    };
    DebugDrawContext& ctx = context();
    for (const auto& edge : e) ctx.add_line(v[edge[0]], v[edge[1]], c, overlay);
}

static void emit_sphere(const glm::vec3& center, float radius, Color c, int segments, bool overlay)
{
    if (segments < 4) segments = 4;
    DebugDrawContext& ctx = context();
    const float two_pi = 6.28318530718f;
    // Three great-circle rings (XY, XZ, YZ) — a cheap readable wireframe sphere.
    for (int ring = 0; ring < 3; ++ring)
    {
        glm::vec3 prev{};
        for (int i = 0; i <= segments; ++i)
        {
            const float t = two_pi * float(i) / float(segments);
            const float s = std::sin(t) * radius, co = std::cos(t) * radius;
            glm::vec3 p;
            if (ring == 0) p = center + glm::vec3{ co, s, 0 };
            else if (ring == 1) p = center + glm::vec3{ co, 0, s };
            else p = center + glm::vec3{ 0, co, s };
            if (i > 0) ctx.add_line(prev, p, c, overlay);
            prev = p;
        }
    }
}

void line(const glm::vec3& a, const glm::vec3& b, Color c) { context().add_line(a, b, c, false); }
void line_overlay(const glm::vec3& a, const glm::vec3& b, Color c) { context().add_line(a, b, c, true); }

void aabb(const glm::vec3& mn, const glm::vec3& mx, Color c) { emit_aabb(mn, mx, c, false); }
void aabb_overlay(const glm::vec3& mn, const glm::vec3& mx, Color c) { emit_aabb(mn, mx, c, true); }

void sphere(const glm::vec3& center, float radius, Color c, int segments)
{
    emit_sphere(center, radius, c, segments, false);
}
void sphere_overlay(const glm::vec3& center, float radius, Color c, int segments)
{
    emit_sphere(center, radius, c, segments, true);
}

void axes(const glm::mat4& transform, float length)
{
    const glm::vec3 o = glm::vec3(transform[3]);
    const glm::vec3 x = glm::vec3(transform[0]) * length;
    const glm::vec3 y = glm::vec3(transform[1]) * length;
    const glm::vec3 z = glm::vec3(transform[2]) * length;
    line(o, o + x, Color{ 255, 40, 40, 255 });
    line(o, o + y, Color{ 40, 255, 40, 255 });
    line(o, o + z, Color{ 60, 120, 255, 255 });
}

void text3d(const glm::vec3& pos, std::string text, Color c)
{
    context().add_text(pos, std::move(text), c);
}

}  // namespace string::debug
