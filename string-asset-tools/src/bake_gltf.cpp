#include <string/asset/tools/bake.hpp>

#include <cstring>
#include <fstream>

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

// File extension for an embedded image's container. Unknown means we could neither read a mime type
// nor recognise the magic bytes, and writing it under a guessed extension would just move the
// failure to the decoder.
const char* extension_for(GltfImageFormat f)
{
    switch (f)
    {
    case GltfImageFormat::Png:  return ".png";
    case GltfImageFormat::Jpeg: return ".jpg";
    case GltfImageFormat::Ktx2: return ".ktx2";
    case GltfImageFormat::Dds:  return ".dds";
    default:                    return nullptr;
    }
}

// EXTRACT an embedded image to a real file beside the glTF, and answer its path relative to
// `base_dir`. The cooked format addresses textures BY PATH, so an image that lives inside the .glb
// has nowhere to go otherwise — it used to be dropped here with a warning, which cost the artist
// the texture and then crashed the engine when the decoder was handed the resulting empty source.
// Extracting makes an embedded image indistinguishable from an external one from this point on: the
// same KTX2/BC7 cook and the same streaming apply, which is what the release requirement needs.
std::string extract_embedded(const GltfTexture& t, std::size_t index,
                             const std::filesystem::path& gltf_path,
                             const std::filesystem::path& base_dir)
{
    const char* ext = extension_for(t.format);
    if (ext == nullptr)
    {
        STRING_LOG_WARN("[cook] embedded texture {}: unrecognised image container ({} bytes), skipped",
                        index, t.encoded.size());
        return {};
    }

    // One directory per source asset, so two glTFs in the same folder cannot collide and the
    // extracted set is obvious to delete/regenerate.
    const std::filesystem::path dir = base_dir / (gltf_path.stem().string() + ".textures");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        STRING_LOG_ERROR("[cook] embedded texture {}: cannot create {}: {}",
                         index, dir.string(), ec.message());
        return {};
    }

    const std::filesystem::path file = dir / ("image" + std::to_string(index) + ext);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(t.encoded.data()),
              static_cast<std::streamsize>(t.encoded.size()));
    if (!out)
    {
        STRING_LOG_ERROR("[cook] embedded texture {}: cannot write {}", index, file.string());
        return {};
    }
    out.close();

    std::filesystem::path rel = std::filesystem::relative(file, base_dir, ec);
    const std::string rel_str = (ec || rel.empty()) ? file.string() : rel.generic_string();
    STRING_LOG_INFO("[cook] embedded texture {} extracted -> {} ({} bytes)",
                    index, rel_str, t.encoded.size());
    return rel_str;
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
    // (a .glb with its images inside) are EXTRACTED to files first — see extract_embedded — so they
    // reach the engine by path like everything else.
    const std::filesystem::path base_dir = gltf_path.parent_path();
    scene.textures.reserve(textures.size());
    for (std::size_t i = 0; i < textures.size(); ++i)
    {
        const GltfTexture& t = textures[i];
        CookedTexture ct{};
        ct.srgb = t.srgb ? 1u : 0u;
        if (!t.file.empty())
        {
            std::error_code ec;
            std::filesystem::path rel = std::filesystem::relative(t.file, base_dir, ec);
            const std::string rel_str = (ec || rel.empty()) ? t.file.string() : rel.generic_string();
            set_texture_path(ct, rel_str);
        }
        else if (!t.encoded.empty())
        {
            const std::string rel_str = extract_embedded(t, i, gltf_path, base_dir);
            if (!rel_str.empty()) set_texture_path(ct, rel_str);
        }
        scene.textures.push_back(ct);
    }

    return scene;
}

}  // namespace string::asset::tools
