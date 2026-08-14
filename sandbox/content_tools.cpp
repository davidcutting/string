#include "content_tools.hpp"

#include <chrono>
#include <cmath>
#include <vector>

#include <glm/gtc/constants.hpp>

#include <string/core/logger.hpp>
#include <string/core/vertex.hpp>

#include <string/asset/manifest.hpp>
#include <string/asset/tools/bake.hpp>

namespace sandbox
{
namespace
{

using string::asset::CookedAlphaMode;
using string::asset::CookedMaterial;
using string::asset::CookedScene;
using string::asset::tools::BakeParams;
using string::asset::tools::GltfDraw;

// Pack a tangent + handedness into the Vertex 10:10:10:2 layout (matches geometry_pass's shader
// unpack; see string/core/vertex.hpp).
uint32_t pack_tangent(const glm::vec3& t, float sign)
{
    const auto sn = [](float v) {
        return static_cast<uint32_t>(
                   static_cast<int32_t>(std::round(glm::clamp(v, -1.0f, 1.0f) * 511.0f)))
               & 0x3FFu;
    };
    const uint32_t w = sign < 0.0f ? 3u : 1u;   // signed 2-bit: 1 -> +1, 3 (-1 as 2-bit) -> -1
    return sn(t.x) | (sn(t.y) << 10) | (sn(t.z) << 20) | (w << 30);
}

// A factor-only material (no textures), CookedMaterial-shaped. Defaults mirror GltfMaterial's.
CookedMaterial factor_material(const glm::vec4& base_color, float metallic, float roughness,
                               CookedAlphaMode alpha_mode = CookedAlphaMode::Opaque,
                               bool double_sided = false)
{
    CookedMaterial m{};
    m.base_color_factor = base_color;
    m.metallic_factor = metallic;
    m.roughness_factor = roughness;
    m.base_color_texture = -1;
    m.metallic_roughness_texture = -1;
    m.normal_texture = -1;
    m.occlusion_texture = -1;
    m.alpha_cutoff = 0.5f;
    m.alpha_mode = static_cast<uint8_t>(alpha_mode);
    m.double_sided = double_sided ? 1 : 0;
    return m;
}

}  // namespace

bool tools_cook_provider::cook_geometry(const std::filesystem::path& source, uint32_t chunk_budget,
                                        string::asset::CookedScene& out)
{
    BakeParams params;
    params.chunk_max_meshlets = chunk_budget;
    try
    {
        out = string::asset::tools::bake_gltf(source, params);
    }
    catch (const std::exception& e)
    {
        STRING_LOG_WARN("[cook] {}: {}", source.string(), e.what());
        return false;
    }
    const std::filesystem::path cooked_path =
        string::asset::cooked_path_for(source, chunk_budget);
    try
    {
        string::asset::tools::write_cooked_file(out, cooked_path);
        string::asset::update_manifest(source, cooked_path, chunk_budget,
                                       out.source_content_hash);
    }
    catch (const std::exception& e)
    {
        STRING_LOG_WARN("[load] could not write cooked file {}: {}", cooked_path.string(),
                        e.what());
    }
    return true;
}

string::assets::asset_id load_lookdev_asset(string::assets::registry& registry)
{
    std::vector<string::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<GltfDraw> draws;
    std::vector<CookedMaterial> materials;

    // Unit-sphere template (UV sphere; normal = position, tangent along +phi).
    constexpr int kStacks = 24, kSlices = 48;
    std::vector<string::Vertex> sphere_verts;
    std::vector<uint32_t> sphere_indices;
    for (int st = 0; st <= kStacks; ++st)
    {
        const float theta = glm::pi<float>() * float(st) / float(kStacks);
        for (int sl = 0; sl <= kSlices; ++sl)
        {
            const float phi = 2.0f * glm::pi<float>() * float(sl) / float(kSlices);
            const glm::vec3 n(std::sin(theta) * std::cos(phi), std::cos(theta),
                              std::sin(theta) * std::sin(phi));
            const glm::vec3 t(-std::sin(phi), 0.0f, std::cos(phi));
            sphere_verts.push_back(string::Vertex{ n, glm::vec3(1.0f),
                { float(sl) / kSlices, float(st) / kStacks }, n, pack_tangent(t, 1.0f) });
        }
    }
    for (int st = 0; st < kStacks; ++st)
        for (int sl = 0; sl < kSlices; ++sl)
        {
            const uint32_t a = uint32_t(st * (kSlices + 1) + sl);
            const uint32_t b = a + kSlices + 1;
            // CCW when viewed from outside (matches the back-face-cull main pipeline).
            sphere_indices.insert(sphere_indices.end(), { a, a + 1, b, b, a + 1, b + 1 });
        }

    const auto add_sphere = [&](const glm::vec3& center, float radius, int material) {
        const uint32_t v0 = static_cast<uint32_t>(vertices.size());
        const uint32_t i0 = static_cast<uint32_t>(indices.size());
        for (string::Vertex v : sphere_verts)
        {
            v.pos = v.pos * radius + center;
            vertices.push_back(v);
        }
        for (uint32_t i : sphere_indices) indices.push_back(i + v0);
        GltfDraw d;
        d.index_offset = i0;
        d.index_count = static_cast<uint32_t>(sphere_indices.size());
        d.material = material;
        d.transform = glm::mat4(1.0f);
        d.aabb_min = center - glm::vec3(radius);
        d.aabb_max = center + glm::vec3(radius);
        draws.push_back(d);
    };
    const auto add_material = [&](glm::vec3 albedo, float metallic, float roughness) {
        materials.push_back(factor_material(glm::vec4(albedo, 1.0f), metallic, roughness));
        return static_cast<int>(materials.size()) - 1;
    };

    // The grid: 8 roughness columns (0..1, perceptual) x 5 metallic rows (0..1), radius-0.5
    // spheres on a wall in the XY plane. Neutral albedo (brighter for metals so the ladder reads).
    constexpr int kCols = 8, kRows = 5;
    constexpr float kSpacing = 1.4f, kRadius = 0.5f;
    for (int row = 0; row < kRows; ++row)
        for (int col = 0; col < kCols; ++col)
        {
            const float metallic = float(row) / float(kRows - 1);
            const float roughness = float(col) / float(kCols - 1);
            const glm::vec3 albedo = glm::mix(glm::vec3(0.5f), glm::vec3(0.9f), metallic);
            add_sphere(glm::vec3((float(col) - (kCols - 1) * 0.5f) * kSpacing,
                                 1.2f + float(row) * kSpacing, 0.0f),
                       kRadius, add_material(albedo, metallic, roughness));
        }
    // The white/mirror pair, on the ground in front of the grid.
    add_sphere(glm::vec3(-1.0f, 0.62f, 2.2f), 0.6f, add_material(glm::vec3(1.0f), 0.0f, 1.0f));
    add_sphere(glm::vec3(1.0f, 0.62f, 2.2f), 0.6f, add_material(glm::vec3(1.0f), 1.0f, 0.0f));

    // Ground slab (two triangles), neutral 40% grey.
    {
        const int mat = static_cast<int>(materials.size());
        materials.push_back(factor_material(glm::vec4(glm::vec3(0.4f), 1.0f), 0.0f, 0.85f));
        const float s = 24.0f;
        const uint32_t v0 = static_cast<uint32_t>(vertices.size());
        const uint32_t i0 = static_cast<uint32_t>(indices.size());
        const glm::vec3 n(0.0f, 1.0f, 0.0f);
        const uint32_t tan = pack_tangent(glm::vec3(1, 0, 0), 1.0f);
        const glm::vec3 corners[4] = { { -s, 0, -s }, { s, 0, -s }, { s, 0, s }, { -s, 0, s } };
        const glm::vec2 uvs[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        for (int i = 0; i < 4; ++i)
            vertices.push_back(string::Vertex{ corners[i], glm::vec3(1.0f), uvs[i], n, tan });
        const uint32_t quad[6] = { v0, v0 + 2, v0 + 1, v0, v0 + 3, v0 + 2 };   // CCW from +Y
        for (uint32_t i : quad) indices.push_back(i);
        GltfDraw d;
        d.index_offset = i0;
        d.index_count = 6;
        d.material = mat;
        d.transform = glm::mat4(1.0f);
        d.aabb_min = { -s, -0.01f, -s };
        d.aabb_max = { s, 0.01f, s };
        draws.push_back(d);
    }

    const auto t0 = std::chrono::steady_clock::now();
    CookedScene cs = string::asset::tools::bake_scene(vertices, indices, draws, BakeParams{});
    cs.materials = std::move(materials);
    const double bake_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    STRING_LOG_INFO("[lookdev] generated + baked in {:.1f} ms", bake_ms);

    return registry.load_baked(std::move(cs), "lookdev");
}

string::assets::asset_id load_transp_test_asset(string::assets::registry& registry)
{
    const glm::vec4 colors[5] = {
        { 1.0f, 0.2f, 0.2f, 0.5f }, { 0.2f, 1.0f, 0.2f, 0.5f }, { 0.2f, 0.2f, 1.0f, 0.5f },
        { 1.0f, 1.0f, 0.2f, 0.5f }, { 1.0f, 0.2f, 1.0f, 0.5f },
    };
    std::vector<string::Vertex> qv;
    std::vector<uint32_t> qi;
    std::vector<GltfDraw> qd;
    std::vector<CookedMaterial> materials;
    for (int q = 0; q < 5; ++q)
    {
        materials.push_back(
            factor_material(colors[q], 1.0f, 1.0f, CookedAlphaMode::Blend, /*double_sided=*/true));

        const float x = 6.0f - static_cast<float>(q) * 1.0f;
        const float cy = 4.5f;
        const float lateral = (static_cast<float>(q) - 2.0f) * 0.4f;
        const float s = 1.6f;
        const uint32_t v0 = static_cast<uint32_t>(qv.size());
        const glm::vec3 nrm{ 1.0f, 0.0f, 0.0f };
        const glm::vec3 col = glm::vec3(colors[q]);
        const glm::vec3 corners[4] = {
            { x, cy - s, lateral - s }, { x, cy - s, lateral + s },
            { x, cy + s, lateral + s }, { x, cy + s, lateral - s },
        };
        for (const glm::vec3& c : corners)
            qv.push_back(string::Vertex{ c, col, { 0.0f, 0.0f }, nrm, 0u });
        const uint32_t idx0 = static_cast<uint32_t>(qi.size());
        const uint32_t quad[6] = { v0, v0 + 1, v0 + 2, v0, v0 + 2, v0 + 3 };
        for (uint32_t k : quad) qi.push_back(k);

        GltfDraw d;
        d.index_offset = idx0;
        d.index_count = 6;
        d.material = q;   // file-local; the registry rebases at insert
        d.transform = glm::mat4(1.0f);
        d.aabb_min = { x - 0.01f, cy - s, lateral - s };
        d.aabb_max = { x + 0.01f, cy + s, lateral + s };
        qd.push_back(d);
    }
    CookedScene cs = string::asset::tools::bake_scene(qv, qi, qd, BakeParams{});
    cs.materials = std::move(materials);
    STRING_LOG_INFO("[brief04] STRING_TRANSP_TEST: injected 5 blended quads (baked)");
    return registry.load_baked(std::move(cs), "transp-test");
}

}  // namespace sandbox
