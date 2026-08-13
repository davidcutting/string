#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include <string/vulkan/render_data.hpp>

namespace string::asset::tools
{

// A reference to a texture's *encoded* source — deliberately not decoded here, so the loader's
// peak memory stays tiny and the pass can decode+upload+free one texture at a time (Sponza's
// textures are multiple GB decoded). Exactly one of `file` / `encoded` is set: external images
// decode from disk, embedded / buffer-view images from these in-memory bytes. `srgb` drives the
// upload VkFormat — base-color maps are sRGB, data maps (normal / metallic-roughness) linear.
// The container an embedded image's bytes are in. Needed because embedded images are EXTRACTED to
// files at bake (see bake_gltf), and a file needs the right extension for the cook's decoder to
// recognise it — the bytes alone do not say what they are.
enum class GltfImageFormat : uint8_t { Unknown = 0, Png, Jpeg, Ktx2, Dds };

struct GltfTexture
{
    std::filesystem::path file;      // non-empty => external image, decode from this path
    std::vector<uint8_t> encoded;    // else => embedded, decode from these encoded bytes
    GltfImageFormat format = GltfImageFormat::Unknown;   // container of `encoded`
    bool srgb = false;
};

// A glTF PBR material. Only the fields the renderer consumes today are kept flat here; texture
// members index into GltfModel::textures (-1 = none). Enough for unlit base-color now, with the
// metallic-roughness / normal slots already carried for when lighting lands.
// glTF alpha rendering mode (KHR core). MASK => alpha-tested cutout (clip on base-color alpha vs
// alpha_cutoff); BLEND => routed to the sorted transparency pass; OPAQUE => alpha ignored.
enum class GltfAlphaMode : uint8_t { Opaque = 0, Mask = 1, Blend = 2 };

struct GltfMaterial
{
    glm::vec4 base_color_factor{ 1.0f };
    float metallic_factor = 1.0f;   // scales the MR texture's blue channel (glTF metalness)
    float roughness_factor = 1.0f;  // scales the MR texture's green channel
    int32_t base_color_texture = -1;
    int32_t metallic_roughness_texture = -1;
    int32_t normal_texture = -1;
    int32_t occlusion_texture = -1;   // glTF occlusion (baked AO); attenuates ambient
    GltfAlphaMode alpha_mode = GltfAlphaMode::Opaque;
    float alpha_cutoff = 0.5f;        // MASK threshold (glTF default 0.5)
    bool double_sided = false;        // disable backface cull + cone cull; flip normal to viewer
};

// One draw call: a contiguous range of the model's shared index buffer, the material to bind,
// the world-space transform of the node that instances this mesh primitive, and the draw's
// world-space AABB (for GPU frustum culling).
struct GltfDraw
{
    uint32_t index_offset = 0;
    uint32_t index_count = 0;
    int32_t material = -1;
    glm::mat4 transform{ 1.0f };
    glm::vec3 aabb_min{ 0.0f };
    glm::vec3 aabb_max{ 0.0f };
    // glTF skin index instancing this primitive, -1 = static (brief 23). Skinned draws carry an
    // IDENTITY transform and a LOCAL-space AABB: glTF specifies that a skinned mesh ignores its
    // node transform — the joint matrices place it in scene space. Baking the node transform
    // anyway (the static path) would double-transform the character.
    int32_t skin = -1;
};

// Geometry flattened from a parsed glTF: a single shared vertex + index buffer (deduplicated
// across instances) and one draw per node-instanced mesh primitive. Coordinates stay in glTF
// space (Y-up, right-handed).
struct GltfGeometry
{
    std::vector<string::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<GltfDraw> draws;
    // Skinning attributes (brief 23), parallel to `vertices`; EMPTY when the source has no
    // skinned primitives. Joints are glTF skin-local indices (u8/u16 sources widened to u16);
    // weights are normalized floats (unorm sources converted by fastgltf). Vertices of static
    // primitives in a mixed file hold zeroes — quantization canonicalizes them.
    std::vector<glm::u16vec4> joints;
    std::vector<glm::vec4> weights;
};

// A parsed glTF: resolved (still-encoded) texture sources + materials, plus an opaque handle to
// the fastgltf asset that flatten_geometry() reads. Parsing is split from flattening so the
// caller can kick off texture decode (from `textures`, on worker threads) while it flattens the
// geometry on the main thread — the two overlap. Move-only; keeps the parsed asset alive until
// flatten_geometry() has run.
struct GltfParsed
{
    std::vector<GltfTexture> textures;
    std::vector<GltfMaterial> materials;

    struct Impl;                 // holds the fastgltf::Asset (kept out of this header)
    std::unique_ptr<Impl> impl;

    GltfParsed();
    ~GltfParsed();
    GltfParsed(GltfParsed&&) noexcept;
    GltfParsed& operator=(GltfParsed&&) noexcept;
};

// Parses a .gltf/.glb file: loads external buffers, resolves (but does not decode) texture
// sources, and reads materials. Throws std::runtime_error on failure.
GltfParsed parse_gltf(const std::filesystem::path& path);

// Flattens the parsed asset's meshes/instances into shared vertex/index buffers + a draw list.
// Takes a non-const ref because fastgltf's scene-node traversal requires a mutable asset.
GltfGeometry flatten_geometry(GltfParsed& parsed);

}  // namespace string::asset::tools
