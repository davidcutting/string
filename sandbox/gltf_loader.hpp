#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <glm/glm.hpp>

#include <string/vulkan/render_data.hpp>

namespace sandbox
{

// A reference to a texture's *encoded* source — deliberately not decoded here, so the loader's
// peak memory stays tiny and the pass can decode+upload+free one texture at a time (Sponza's
// textures are multiple GB decoded). Exactly one of `file` / `encoded` is set: external images
// decode from disk, embedded / buffer-view images from these in-memory bytes. `srgb` drives the
// upload VkFormat — base-color maps are sRGB, data maps (normal / metallic-roughness) linear.
struct GltfTexture
{
    std::filesystem::path file;      // non-empty => external image, decode from this path
    std::vector<uint8_t> encoded;    // else => embedded, decode from these encoded bytes
    bool srgb = false;
};

// A glTF PBR material. Only the fields the renderer consumes today are kept flat here; texture
// members index into GltfModel::textures (-1 = none). Enough for unlit base-color now, with the
// metallic-roughness / normal slots already carried for when lighting lands.
struct GltfMaterial
{
    glm::vec4 base_color_factor{ 1.0f };
    int32_t base_color_texture = -1;
    int32_t metallic_roughness_texture = -1;
    int32_t normal_texture = -1;
};

// One draw call: a contiguous range of the model's shared index buffer, the material to bind,
// and the world-space transform of the node that instances this mesh primitive.
struct GltfDraw
{
    uint32_t index_offset = 0;
    uint32_t index_count = 0;
    int32_t material = -1;
    glm::mat4 transform{ 1.0f };
};

// A glTF scene flattened into GPU-ready form: a single shared vertex + index buffer (geometry
// deduplicated across instances), one draw per node-instanced mesh primitive, and the decoded
// materials + textures they reference. Coordinates stay in glTF space (Y-up, right-handed).
struct GltfModel
{
    std::vector<String::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<GltfDraw> draws;
    std::vector<GltfMaterial> materials;
    std::vector<GltfTexture> textures;
};

// Parses a .gltf/.glb file into a GltfModel. Throws std::runtime_error on parse failure.
// External buffers (.bin) are loaded; texture *sources* are resolved but not decoded (the pass
// streams them one at a time — see GltfTexture).
GltfModel load_gltf(const std::filesystem::path& path);

}  // namespace sandbox
