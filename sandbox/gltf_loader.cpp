#include "gltf_loader.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include <string/core/logger.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/glm_element_traits.hpp>   // ElementTraits for glm::vec2/3/4 accessor reads

namespace sandbox
{
namespace
{

// Copy a slice of encoded image bytes into a texture source (no decode — the pass decodes it).
GltfTexture encoded_source(const std::byte* bytes, std::size_t size)
{
    GltfTexture texture;
    const auto* begin = reinterpret_cast<const uint8_t*>(bytes);
    texture.encoded.assign(begin, begin + size);
    return texture;
}

// Resolve one glTF image to an *undecoded* source: a file path for external images, or a copy of
// the encoded bytes for embedded / buffer-view images. Decoding is deferred to the pass so only
// one texture is ever resident in decoded form at a time.
GltfTexture resolve_image(const fastgltf::Asset& asset, const fastgltf::Image& image,
                          const std::filesystem::path& base_dir)
{
    GltfTexture result;
    std::visit(fastgltf::visitor{
        [](const auto&) { throw std::runtime_error("gltf: unsupported image data source"); },
        [&](const fastgltf::sources::URI& source) {
            if (source.fileByteOffset != 0)
            {
                throw std::runtime_error("gltf: image URIs with a byte offset are unsupported");
            }
            result.file = base_dir / source.uri.fspath();
        },
        [&](const fastgltf::sources::Array& source) {
            result = encoded_source(source.bytes.data(), source.bytes.size());
        },
        [&](const fastgltf::sources::Vector& source) {
            result = encoded_source(source.bytes.data(), source.bytes.size());
        },
        [&](const fastgltf::sources::BufferView& source) {
            const auto& view = asset.bufferViews[source.bufferViewIndex];
            const auto& buffer = asset.buffers[view.bufferIndex];
            std::visit(fastgltf::visitor{
                [](const auto&) { throw std::runtime_error("gltf: unsupported buffer source for image"); },
                [&](const fastgltf::sources::Array& bytes) {
                    result = encoded_source(bytes.bytes.data() + view.byteOffset, view.byteLength);
                },
                [&](const fastgltf::sources::Vector& bytes) {
                    result = encoded_source(bytes.bytes.data() + view.byteOffset, view.byteLength);
                },
                [&](const fastgltf::sources::ByteView& bytes) {
                    result = encoded_source(bytes.bytes.data() + view.byteOffset, view.byteLength);
                },
            }, buffer.data);
        },
    }, image.data);
    return result;
}

// The GltfModel::textures index behind a material texture slot, or -1. glTF indirects
// material -> texture -> image; we key our decoded textures by image index.
template <typename OptTextureInfo>
int32_t image_index_of(const fastgltf::Asset& asset, const OptTextureInfo& info)
{
    if (!info.has_value())
    {
        return -1;
    }
    const auto& texture = asset.textures[info->textureIndex];
    return texture.imageIndex.has_value() ? static_cast<int32_t>(texture.imageIndex.value()) : -1;
}

// A mesh primitive's slice of the shared index buffer + its material, recorded once per mesh so
// node instances can reference the same geometry under different transforms. Carries the
// primitive's local-space AABB so each node instance can derive a world-space AABB for culling.
struct MeshPrimitive
{
    uint32_t index_offset;
    uint32_t index_count;
    int32_t material;
    glm::vec3 local_min;
    glm::vec3 local_max;
};

}  // namespace

// Holds the parsed fastgltf asset alive between parse_gltf() and flatten_geometry(), keeping
// fastgltf out of the public header.
struct GltfParsed::Impl
{
    fastgltf::Asset asset;
};

GltfParsed::GltfParsed() = default;
GltfParsed::~GltfParsed() = default;
GltfParsed::GltfParsed(GltfParsed&&) noexcept = default;
GltfParsed& GltfParsed::operator=(GltfParsed&&) noexcept = default;

GltfParsed parse_gltf(const std::filesystem::path& path)
{
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None)
    {
        throw std::runtime_error("gltf: cannot open " + path.string());
    }

    const auto parse_start = std::chrono::steady_clock::now();
    fastgltf::Parser parser;
    auto loaded = parser.loadGltf(data.get(), path.parent_path(), fastgltf::Options::LoadExternalBuffers);
    if (loaded.error() != fastgltf::Error::None)
    {
        throw std::runtime_error("gltf: failed to parse " + path.string() + ": " +
                                 std::string(fastgltf::getErrorMessage(loaded.error())));
    }

    GltfParsed parsed;
    parsed.impl = std::make_unique<GltfParsed::Impl>();
    parsed.impl->asset = std::move(loaded.get());
    const fastgltf::Asset& asset = parsed.impl->asset;

    const auto parse_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - parse_start).count();
    STRING_LOG_INFO("[load]   fastgltf parse + read buffers: {} ms", parse_ms);

    const std::filesystem::path base_dir = path.parent_path();

    // --- Textures (resolve sources only; the pass decodes + uploads them) ---------------------
    parsed.textures.reserve(asset.images.size());
    for (const auto& image : asset.images)
    {
        parsed.textures.push_back(resolve_image(asset, image, base_dir));
    }

    // --- Materials ---------------------------------------------------------------------------
    parsed.materials.reserve(asset.materials.size());
    for (const auto& material : asset.materials)
    {
        GltfMaterial out;
        const auto& factor = material.pbrData.baseColorFactor;
        out.base_color_factor = { factor[0], factor[1], factor[2], factor[3] };
        out.base_color_texture = image_index_of(asset, material.pbrData.baseColorTexture);
        out.metallic_roughness_texture = image_index_of(asset, material.pbrData.metallicRoughnessTexture);
        out.normal_texture = image_index_of(asset, material.normalTexture);

        // Base-color maps carry sRGB-encoded color; data maps stay linear. (Left at the default
        // linear for normal / metallic-roughness.)
        if (out.base_color_texture >= 0)
        {
            parsed.textures[out.base_color_texture].srgb = true;
        }
        parsed.materials.push_back(out);
    }

    return parsed;
}

GltfGeometry flatten_geometry(GltfParsed& parsed)
{
    fastgltf::Asset& asset = parsed.impl->asset;
    const auto flatten_start = std::chrono::steady_clock::now();

    GltfGeometry model;

    // --- Geometry: flatten every mesh primitive into the shared vertex/index buffers ---------
    // Pre-pass: sum the total vertex + index counts across all primitives and reserve once.
    // Growing the shared buffers per primitive (resize/reserve to the exact new size, ~hundreds
    // of times) reallocates and copies the whole buffer each time — O(n^2), and the dominant
    // load cost on large scenes (Sponza: ~2M verts / 11M indices). One reservation makes the
    // per-primitive resize()s stay within capacity (no reallocation).
    std::size_t total_vertices = 0;
    std::size_t total_indices = 0;
    for (const auto& mesh : asset.meshes)
    {
        for (const auto& primitive : mesh.primitives)
        {
            if (primitive.type != fastgltf::PrimitiveType::Triangles)
            {
                continue;
            }
            const auto* position = primitive.findAttribute("POSITION");
            if (position == primitive.attributes.end())
            {
                continue;
            }
            const auto& position_accessor = asset.accessors[position->accessorIndex];
            total_vertices += position_accessor.count;
            total_indices += primitive.indicesAccessor.has_value()
                ? asset.accessors[primitive.indicesAccessor.value()].count
                : position_accessor.count;
        }
    }
    model.vertices.reserve(total_vertices);
    model.indices.reserve(total_indices);

    std::vector<std::vector<MeshPrimitive>> mesh_primitives(asset.meshes.size());
    for (std::size_t mesh_index = 0; mesh_index < asset.meshes.size(); ++mesh_index)
    {
        for (const auto& primitive : asset.meshes[mesh_index].primitives)
        {
            if (primitive.type != fastgltf::PrimitiveType::Triangles)
            {
                continue;   // only triangle lists for now
            }
            const auto* position = primitive.findAttribute("POSITION");
            if (position == primitive.attributes.end())
            {
                continue;
            }

            const auto& position_accessor = asset.accessors[position->accessorIndex];
            const uint32_t base_vertex = static_cast<uint32_t>(model.vertices.size());
            model.vertices.resize(base_vertex + position_accessor.count);

            // Accumulate the primitive's local-space AABB from its positions (for culling).
            glm::vec3 local_min(std::numeric_limits<float>::max());
            glm::vec3 local_max(std::numeric_limits<float>::lowest());
            fastgltf::iterateAccessorWithIndex<glm::vec3>(
                asset, position_accessor, [&](glm::vec3 value, std::size_t i) {
                    model.vertices[base_vertex + i].pos = value;
                    model.vertices[base_vertex + i].color = glm::vec3(1.0f);
                    local_min = glm::min(local_min, value);
                    local_max = glm::max(local_max, value);
                });

            if (const auto* normal = primitive.findAttribute("NORMAL"); normal != primitive.attributes.end())
            {
                fastgltf::iterateAccessorWithIndex<glm::vec3>(
                    asset, asset.accessors[normal->accessorIndex], [&](glm::vec3 value, std::size_t i) {
                        model.vertices[base_vertex + i].normal = value;
                    });
            }

            if (const auto* uv = primitive.findAttribute("TEXCOORD_0"); uv != primitive.attributes.end())
            {
                fastgltf::iterateAccessorWithIndex<glm::vec2>(
                    asset, asset.accessors[uv->accessorIndex], [&](glm::vec2 value, std::size_t i) {
                        model.vertices[base_vertex + i].texCoord = value;
                    });
            }

            const uint32_t index_offset = static_cast<uint32_t>(model.indices.size());
            if (primitive.indicesAccessor.has_value())
            {
                const auto& index_accessor = asset.accessors[primitive.indicesAccessor.value()];
                model.indices.reserve(model.indices.size() + index_accessor.count);
                fastgltf::iterateAccessor<uint32_t>(asset, index_accessor, [&](uint32_t index) {
                    model.indices.push_back(base_vertex + index);
                });
            }
            else
            {
                // Non-indexed primitive: synthesise a trivial index run.
                for (uint32_t i = 0; i < position_accessor.count; ++i)
                {
                    model.indices.push_back(base_vertex + i);
                }
            }

            const uint32_t index_count = static_cast<uint32_t>(model.indices.size()) - index_offset;
            const int32_t material = primitive.materialIndex.has_value()
                ? static_cast<int32_t>(primitive.materialIndex.value())
                : -1;
            mesh_primitives[mesh_index].push_back(
                { index_offset, index_count, material, local_min, local_max });
        }
    }

    // --- Instances: one draw per node-instanced mesh primitive, world transform baked in ------
    const std::size_t scene_index = asset.defaultScene.value_or(0);
    if (scene_index < asset.scenes.size())
    {
        fastgltf::iterateSceneNodes(
            asset, scene_index, fastgltf::math::fmat4x4(),
            [&](fastgltf::Node& node, const fastgltf::math::fmat4x4& world) {
                if (!node.meshIndex.has_value())
                {
                    return;
                }
                const glm::mat4 transform = glm::make_mat4(world.data());
                for (const auto& primitive : mesh_primitives[node.meshIndex.value()])
                {
                    // World-space AABB: transform the 8 corners of the local AABB and re-bound.
                    glm::vec3 world_min(std::numeric_limits<float>::max());
                    glm::vec3 world_max(std::numeric_limits<float>::lowest());
                    for (int corner = 0; corner < 8; ++corner)
                    {
                        const glm::vec3 local = {
                            (corner & 1) ? primitive.local_max.x : primitive.local_min.x,
                            (corner & 2) ? primitive.local_max.y : primitive.local_min.y,
                            (corner & 4) ? primitive.local_max.z : primitive.local_min.z,
                        };
                        const glm::vec3 world_corner = glm::vec3(transform * glm::vec4(local, 1.0f));
                        world_min = glm::min(world_min, world_corner);
                        world_max = glm::max(world_max, world_corner);
                    }
                    model.draws.push_back(GltfDraw{
                        primitive.index_offset,
                        primitive.index_count,
                        primitive.material,
                        transform,
                        world_min,
                        world_max,
                    });
                }
            });
    }

    const auto flatten_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - flatten_start).count();
    STRING_LOG_INFO("[load]   flatten geometry ({} verts, {} indices): {} ms",
                    model.vertices.size(), model.indices.size(), flatten_ms);

    return model;
}

}  // namespace sandbox
