// A node transform with a NEGATIVE DETERMINANT (a -1 scale, an applied Mirror modifier) reverses
// triangle orientation. Every pipeline in the engine rasterises VK_FRONT_FACE_COUNTER_CLOCKWISE, so
// a mirrored draw's triangles came out clockwise and were back-face culled — geometry that looks
// correct in Blender arriving inside-out, a few faces at a time.
//
// These tests assert the invariant the renderer actually depends on: after a draw's model matrix is
// applied, every baked triangle faces the same way. That is checked on the DECODED meshlet output,
// so it covers the winding through meshletization rather than trusting an intermediate.

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>   // glm::scale — was arriving transitively via render_data.hpp

#include <string/asset/tools/bake.hpp>
#include <string/asset/cooked_format.hpp>

using namespace string::asset;
using namespace string::asset::tools;

namespace
{

struct Scene
{
    std::vector<string::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<GltfDraw> draws;
};

// One flat quad in the XZ plane, wound counter-clockwise as seen from +Y, plus a draw that
// instances it under `transform`. Enough triangles to survive meshletization intact.
void add_quad_grid(Scene& s, const glm::mat4& transform, int n = 8)
{
    const uint32_t base_vertex = static_cast<uint32_t>(s.vertices.size());
    const uint32_t index_offset = static_cast<uint32_t>(s.indices.size());
    glm::vec3 mn(1e30f), mx(-1e30f);
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x)
        {
            string::Vertex v{};
            v.pos = glm::vec3(float(x), 0.0f, float(y));
            v.color = glm::vec3(1.0f);
            v.texCoord = glm::vec2(float(x) / n, float(y) / n);
            v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            v.tangent = 0x40100401u;
            s.vertices.push_back(v);
            mn = glm::min(mn, v.pos);
            mx = glm::max(mx, v.pos);
        }
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
        {
            const uint32_t a = base_vertex + uint32_t(y * (n + 1) + x);
            const uint32_t b = a + 1;
            const uint32_t c = a + uint32_t(n + 1);
            const uint32_t d = c + 1;
            // CCW from +Y: cross(b-a, c-a) points along +Y.
            s.indices.insert(s.indices.end(), { a, c, b, b, c, d });
        }

    GltfDraw draw{};
    draw.index_offset = index_offset;
    draw.index_count = static_cast<uint32_t>(s.indices.size()) - index_offset;
    draw.material = 0;
    draw.transform = transform;
    draw.aabb_min = mn;
    draw.aabb_max = mx;
    s.draws.push_back(draw);
}

// Every baked triangle of every meshlet, as world-space geometric normals under `transform`.
// Walks LOD0 only: the LOD chain is simplified geometry and is not what the artist sees up close.
std::vector<glm::vec3> lod0_triangle_normals(const CookedScene& cooked, std::size_t draw_index,
                                             const std::vector<string::Vertex>& verts,
                                             const glm::mat4& transform)
{
    std::vector<glm::vec3> normals;
    const CookedDraw& draw = cooked.draws[draw_index];
    const GpuMeshletLod& lod = draw.lods[0];
    for (uint32_t m = 0; m < lod.meshlet_count; ++m)
    {
        const GpuMeshlet& ml = cooked.meshlets[lod.meshlet_offset + m];
        for (uint32_t t = 0; t < ml.triangle_count; ++t)
        {
            const uint32_t word = cooked.meshlet_triangles[ml.triangle_offset + t];
            const uint32_t li[3] = { word & 0xFF, (word >> 8) & 0xFF, (word >> 16) & 0xFF };
            glm::vec3 p[3];
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t global = cooked.meshlet_vertices[ml.vertex_offset + li[k]];
                p[k] = glm::vec3(transform * glm::vec4(verts[global].pos, 1.0f));
            }
            const glm::vec3 n = glm::cross(p[1] - p[0], p[2] - p[0]);
            if (glm::length(n) > 1e-12f) normals.push_back(glm::normalize(n));
        }
    }
    return normals;
}

CookedScene bake(const Scene& s)
{
    return bake_scene(s.vertices, s.indices, s.draws, BakeParams{});
}

}  // namespace

// Baseline: an unmirrored draw must come out facing +Y. If this ever fails the test itself is wrong.
TEST(Winding, UnmirroredDrawFacesForward)
{
    Scene s;
    const glm::mat4 identity(1.0f);
    add_quad_grid(s, identity);

    const CookedScene cooked = bake(s);
    const std::vector<glm::vec3> normals = lod0_triangle_normals(cooked, 0, s.vertices, identity);

    ASSERT_FALSE(normals.empty());
    for (const glm::vec3& n : normals) EXPECT_GT(n.y, 0.0f);
}

// The bug: mirrored on X. Before the fix every one of these faced -Y and was culled.
TEST(Winding, MirroredDrawStillFacesForward)
{
    Scene s;
    const glm::mat4 mirror = glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f));
    add_quad_grid(s, mirror);

    const CookedScene cooked = bake(s);
    const std::vector<glm::vec3> normals = lod0_triangle_normals(cooked, 0, s.vertices, mirror);

    ASSERT_FALSE(normals.empty());
    for (const glm::vec3& n : normals)
        EXPECT_GT(n.y, 0.0f) << "mirrored draw is inside-out";
}

// A mirror on any single axis flips orientation; a mirror on TWO axes is a rotation and must be
// left alone. Reversing on "has a negative scale component" rather than on the determinant would
// break this one.
TEST(Winding, TwoAxisMirrorIsARotationAndIsNotReversed)
{
    Scene s;
    const glm::mat4 rot = glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, -1.0f));
    add_quad_grid(s, rot);

    const CookedScene cooked = bake(s);
    const std::vector<glm::vec3> normals = lod0_triangle_normals(cooked, 0, s.vertices, rot);

    ASSERT_FALSE(normals.empty());
    for (const glm::vec3& n : normals)
        EXPECT_GT(n.y, 0.0f) << "a two-axis mirror is a rotation; winding must not be touched";
}

// The same mesh instanced both ways in one scene: the fix must be per-draw, not global. Each draw
// owns its index copy, so one is reversed and the other is not.
TEST(Winding, MirroredAndUnmirroredInstancesCoexist)
{
    Scene s;
    const glm::mat4 identity(1.0f);
    const glm::mat4 mirror = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f));
    add_quad_grid(s, identity);
    add_quad_grid(s, mirror);

    const CookedScene cooked = bake(s);
    ASSERT_GE(cooked.draws.size(), 2u);

    for (const glm::vec3& n : lod0_triangle_normals(cooked, 0, s.vertices, identity))
        EXPECT_GT(n.y, 0.0f) << "unmirrored instance was disturbed";
    for (const glm::vec3& n : lod0_triangle_normals(cooked, 1, s.vertices, mirror))
        EXPECT_GT(n.y, 0.0f) << "mirrored instance is inside-out";
}

// The bake is content-addressed and must stay deterministic with the reversal in place.
TEST(Winding, MirroredBakeIsDeterministic)
{
    Scene s;
    add_quad_grid(s, glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f)));

    EXPECT_EQ(write_cooked(bake(s)), write_cooked(bake(s)));
}
