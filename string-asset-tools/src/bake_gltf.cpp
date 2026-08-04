#include <string/asset/tools/bake.hpp>

#include <cstring>

#include <string/core/logger.hpp>

namespace string::asset::tools
{
namespace
{

// Copy a relative path string into a CookedTexture's fixed inline buffer (NUL-terminated, zero-
// padded so the struct's bytes are deterministic). Truncates + warns on overflow (paths are short).
void set_texture_path(CookedTexture& t, const std::string& rel)
{
    std::memset(t.path, 0, sizeof(t.path));
    if (rel.size() >= kCookedTexturePathMax)
    {
        STRING_LOG_WARN("[cook] texture path too long, truncated: {}", rel);
        std::memcpy(t.path, rel.data(), kCookedTexturePathMax - 1);
    }
    else
    {
        std::memcpy(t.path, rel.data(), rel.size());
    }
}

CookedAlphaMode to_cooked_alpha(GltfAlphaMode m)
{
    switch (m)
    {
    case GltfAlphaMode::Mask:  return CookedAlphaMode::Mask;
    case GltfAlphaMode::Blend: return CookedAlphaMode::Blend;
    default:                   return CookedAlphaMode::Opaque;
    }
}

}  // namespace

CookedScene bake_gltf(const std::filesystem::path& gltf_path, const BakeParams& params)
{
    GltfParsed parsed = parse_gltf(gltf_path);
    // Snapshot materials/textures before flatten (flatten needs a mutable ref but leaves these).
    std::vector<GltfMaterial> materials = parsed.materials;
    std::vector<GltfTexture> textures = parsed.textures;
    GltfGeometry geometry = flatten_geometry(parsed);

    CookedScene scene = bake_scene(geometry.vertices, geometry.indices, geometry.draws, params);

    // Materials (texture indices are file-local; the engine rebases them at merge, same as before).
    scene.materials.reserve(materials.size());
    for (const GltfMaterial& m : materials)
    {
        CookedMaterial cm{};
        cm.base_color_factor = m.base_color_factor;
        cm.metallic_factor = m.metallic_factor;
        cm.roughness_factor = m.roughness_factor;
        cm.base_color_texture = m.base_color_texture;
        cm.metallic_roughness_texture = m.metallic_roughness_texture;
        cm.normal_texture = m.normal_texture;
        cm.occlusion_texture = m.occlusion_texture;
        cm.alpha_cutoff = m.alpha_cutoff;
        cm.alpha_mode = static_cast<uint8_t>(to_cooked_alpha(m.alpha_mode));
        cm.double_sided = m.double_sided ? 1u : 0u;
        scene.materials.push_back(cm);
    }

    // Textures: store each source image's path RELATIVE to the glTF's directory (so the cooked file
    // is relocatable with its assets) + the srgb flag. The KTX2/BC7 cook is unchanged; the engine
    // applies the `.ktx2` sibling resolution + streaming at load exactly as before. Embedded textures
    // (no file path) can't be referenced by path — they keep an empty path and the engine falls back
    // (Sponza has none; brief 04b keeps textures as-is).
    const std::filesystem::path base_dir = gltf_path.parent_path();
    scene.textures.reserve(textures.size());
    for (const GltfTexture& t : textures)
    {
        CookedTexture ct{};
        ct.srgb = t.srgb ? 1u : 0u;
        if (!t.file.empty())
        {
            std::error_code ec;
            std::filesystem::path rel = std::filesystem::relative(t.file, base_dir, ec);
            const std::string rel_str = (ec || rel.empty()) ? t.file.string() : rel.generic_string();
            set_texture_path(ct, rel_str);
        }
        else
        {
            STRING_LOG_WARN("[cook] embedded texture has no file path; engine will use fallback");
        }
        scene.textures.push_back(ct);
    }

    return scene;
}

}  // namespace string::asset::tools
