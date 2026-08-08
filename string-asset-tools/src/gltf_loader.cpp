#include <string/asset/tools/gltf_loader.hpp>

#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <string/core/job_system.hpp>
#include <string/core/logger.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/glm_element_traits.hpp>   // ElementTraits for glm::vec2/3/4 accessor reads

#include "third_party/mikktspace/mikktspace.h"

namespace string::asset::tools
{
namespace
{

// Pack a normalized tangent + handedness sign into the vertex's 10:10:10:2 field (matching
// VK_FORMAT_A2B10G10R10_SNORM_PACK32 / the shader unpack): xyz as 10-bit snorm, w=sign in 2 bits.
uint32_t pack_tangent(const glm::vec3& t, float sign)
{
    auto snorm10 = [](float v) -> uint32_t {
        v = glm::clamp(v, -1.0f, 1.0f);
        const int32_t q = static_cast<int32_t>(std::lround(v * 511.0f));
        return static_cast<uint32_t>(q & 0x3FF);
    };
    // A2B10G10R10: bits [0..9]=x, [10..19]=y, [20..29]=z, [30..31]=w. A 2-bit snorm stores +1 as 01
    // and -1 as 11 (== -1 in 2-bit two's complement); the shader reads .w and does sign(w) or w<0.
    const uint32_t w2 = sign < 0.0f ? 0x3u : 0x1u;
    return snorm10(t.x) | (snorm10(t.y) << 10) | (snorm10(t.z) << 20) | (w2 << 30);
}

// MikkTSpace generation context: runs over a de-indexed triangle list for one primitive's vertex
// range, then writes packed tangents back to the (indexed) shared vertex array. glTF winds CCW.
struct MikktMesh
{
    string::Vertex* verts;          // base of this primitive's vertex range in model.vertices
    const uint32_t* indices;        // global index values for this primitive (already vertex-rebased)
    uint32_t base_vertex;           // subtract to map a global index back to a local vertex slot
    uint32_t face_count;
};

int mikkt_num_faces(const SMikkTSpaceContext* c)
{
    return static_cast<const MikktMesh*>(c->m_pUserData)->face_count;
}
int mikkt_num_verts_of_face(const SMikkTSpaceContext*, int) { return 3; }

const string::Vertex& mikkt_vertex(const SMikkTSpaceContext* c, int face, int vert)
{
    const MikktMesh* m = static_cast<const MikktMesh*>(c->m_pUserData);
    const uint32_t idx = m->indices[face * 3 + vert] - m->base_vertex;
    return m->verts[idx];
}
void mikkt_get_position(const SMikkTSpaceContext* c, float out[], int face, int vert)
{
    const glm::vec3& p = mikkt_vertex(c, face, vert).pos;
    out[0] = p.x; out[1] = p.y; out[2] = p.z;
}
void mikkt_get_normal(const SMikkTSpaceContext* c, float out[], int face, int vert)
{
    const glm::vec3& n = mikkt_vertex(c, face, vert).normal;
    out[0] = n.x; out[1] = n.y; out[2] = n.z;
}
void mikkt_get_texcoord(const SMikkTSpaceContext* c, float out[], int face, int vert)
{
    const glm::vec2& uv = mikkt_vertex(c, face, vert).texCoord;
    out[0] = uv.x; out[1] = uv.y;
}
void mikkt_set_tspace_basic(const SMikkTSpaceContext* c, const float t[], float sign, int face, int vert)
{
    const MikktMesh* m = static_cast<const MikktMesh*>(c->m_pUserData);
    const uint32_t idx = m->indices[face * 3 + vert] - m->base_vertex;
    // MikkTSpace's sign is the bitangent handedness (B = sign * cross(N, T)); pass it through.
    m->verts[idx].tangent = pack_tangent(glm::vec3(t[0], t[1], t[2]), sign);
}

GltfImageFormat format_of(fastgltf::MimeType mime)
{
    switch (mime)
    {
    case fastgltf::MimeType::PNG:  return GltfImageFormat::Png;
    case fastgltf::MimeType::JPEG: return GltfImageFormat::Jpeg;
    case fastgltf::MimeType::KTX2: return GltfImageFormat::Ktx2;
    case fastgltf::MimeType::DDS:  return GltfImageFormat::Dds;
    default:                       return GltfImageFormat::Unknown;
    }
}

// Sniff the container from the leading magic bytes. The mime type is OPTIONAL on a buffer-view
// image and Blender's GLB exporter routinely omits it, so trusting it alone would leave real
// embedded textures unnamed and therefore unextractable.
GltfImageFormat sniff_format(const std::vector<uint8_t>& bytes)
{
    static constexpr uint8_t kPng[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (bytes.size() >= 8 && std::equal(std::begin(kPng), std::end(kPng), bytes.begin()))
        return GltfImageFormat::Png;
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF)
        return GltfImageFormat::Jpeg;
    if (bytes.size() >= 12 && bytes[0] == 0xAB && bytes[1] == 'K' && bytes[2] == 'T' && bytes[3] == 'X')
        return GltfImageFormat::Ktx2;
    if (bytes.size() >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ')
        return GltfImageFormat::Dds;
    return GltfImageFormat::Unknown;
}

// Copy a slice of encoded image bytes into a texture source (no decode — the pass decodes it).
GltfTexture encoded_source(const std::byte* bytes, std::size_t size, fastgltf::MimeType mime)
{
    GltfTexture texture;
    const auto* begin = reinterpret_cast<const uint8_t*>(bytes);
    texture.encoded.assign(begin, begin + size);
    texture.format = format_of(mime);
    if (texture.format == GltfImageFormat::Unknown) texture.format = sniff_format(texture.encoded);
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
            result = encoded_source(source.bytes.data(), source.bytes.size(), source.mimeType);
        },
        [&](const fastgltf::sources::Vector& source) {
            result = encoded_source(source.bytes.data(), source.bytes.size(), source.mimeType);
        },
        [&](const fastgltf::sources::BufferView& source) {
            const auto& view = asset.bufferViews[source.bufferViewIndex];
            const auto& buffer = asset.buffers[view.bufferIndex];
            // The IMAGE's mime type, not the buffer's: the buffer is an octet-stream either way.
            const fastgltf::MimeType mime = source.mimeType;
            std::visit(fastgltf::visitor{
                [](const auto&) { throw std::runtime_error("gltf: unsupported buffer source for image"); },
                [&](const fastgltf::sources::Array& bytes) {
                    result = encoded_source(bytes.bytes.data() + view.byteOffset, view.byteLength, mime);
                },
                [&](const fastgltf::sources::Vector& bytes) {
                    result = encoded_source(bytes.bytes.data() + view.byteOffset, view.byteLength, mime);
                },
                [&](const fastgltf::sources::ByteView& bytes) {
                    result = encoded_source(bytes.bytes.data() + view.byteOffset, view.byteLength, mime);
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
        out.metallic_factor = material.pbrData.metallicFactor;
        out.roughness_factor = material.pbrData.roughnessFactor;
        out.base_color_texture = image_index_of(asset, material.pbrData.baseColorTexture);
        out.metallic_roughness_texture = image_index_of(asset, material.pbrData.metallicRoughnessTexture);
        out.normal_texture = image_index_of(asset, material.normalTexture);
        out.occlusion_texture = image_index_of(asset, material.occlusionTexture);

        switch (material.alphaMode)
        {
        case fastgltf::AlphaMode::Mask:  out.alpha_mode = GltfAlphaMode::Mask;  break;
        case fastgltf::AlphaMode::Blend: out.alpha_mode = GltfAlphaMode::Blend; break;
        default:                         out.alpha_mode = GltfAlphaMode::Opaque; break;
        }
        out.alpha_cutoff = material.alphaCutoff;
        out.double_sided = material.doubleSided;

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
    // Plan pass (sequential, cheap): assign each triangle primitive a disjoint slice of the shared
    // vertex/index buffers via running prefix sums, and size both buffers exactly once. Because the
    // slices don't overlap, the expensive attribute copies below can then run in parallel across a
    // worker pool with no locking — this is the dominant load cost on large scenes (Sponza: ~2M
    // verts / 11M indices), previously all on one thread.
    struct PrimitivePlan
    {
        std::size_t mesh_index;
        std::size_t primitive_index;   // index within mesh.primitives
        uint32_t base_vertex;
        uint32_t vertex_count;
        uint32_t index_offset;
        uint32_t index_count;
        glm::vec3 local_min{ 0.0f };   // primitive's local-space AABB, filled by the parallel pass
        glm::vec3 local_max{ 0.0f };
        bool has_tangent = false;      // glTF supplied TANGENT (else MikkTSpace generates it)
    };

    std::vector<PrimitivePlan> plans;
    std::size_t total_vertices = 0;
    std::size_t total_indices = 0;
    for (std::size_t mesh_index = 0; mesh_index < asset.meshes.size(); ++mesh_index)
    {
        const auto& mesh = asset.meshes[mesh_index];
        for (std::size_t p = 0; p < mesh.primitives.size(); ++p)
        {
            const auto& primitive = mesh.primitives[p];
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
            const uint32_t vertex_count = static_cast<uint32_t>(position_accessor.count);
            const uint32_t index_count = primitive.indicesAccessor.has_value()
                ? static_cast<uint32_t>(asset.accessors[primitive.indicesAccessor.value()].count)
                : vertex_count;
            plans.push_back(PrimitivePlan{
                .mesh_index = mesh_index,
                .primitive_index = p,
                .base_vertex = static_cast<uint32_t>(total_vertices),
                .vertex_count = vertex_count,
                .index_offset = static_cast<uint32_t>(total_indices),
                .index_count = index_count,
            });
            total_vertices += vertex_count;
            total_indices += index_count;
        }
    }
    model.vertices.resize(total_vertices);
    model.indices.resize(total_indices);

    // Parallel fill: each primitive writes only its own [base_vertex, +count) vertex range and
    // [index_offset, +count) index range, so the workers never touch the same element. Reads of
    // the (const) asset are thread-safe. Each job also records its primitive's local AABB.
    {
        const fastgltf::Asset& casset = asset;
        string::core::job_system pool;
        std::vector<std::future<void>> jobs;
        jobs.reserve(plans.size());
        for (PrimitivePlan& plan : plans)
        {
            jobs.push_back(pool.enqueue([&casset, &model, &plan]() {
                const auto& primitive = casset.meshes[plan.mesh_index].primitives[plan.primitive_index];
                const auto& position_accessor = casset.accessors[
                    primitive.findAttribute("POSITION")->accessorIndex];

                glm::vec3 local_min(std::numeric_limits<float>::max());
                glm::vec3 local_max(std::numeric_limits<float>::lowest());
                fastgltf::iterateAccessorWithIndex<glm::vec3>(
                    casset, position_accessor, [&](glm::vec3 value, std::size_t i) {
                        string::Vertex& v = model.vertices[plan.base_vertex + i];
                        v.pos = value;
                        v.color = glm::vec3(1.0f);
                        local_min = glm::min(local_min, value);
                        local_max = glm::max(local_max, value);
                    });

                if (const auto* normal = primitive.findAttribute("NORMAL"); normal != primitive.attributes.end())
                {
                    fastgltf::iterateAccessorWithIndex<glm::vec3>(
                        casset, casset.accessors[normal->accessorIndex], [&](glm::vec3 value, std::size_t i) {
                            model.vertices[plan.base_vertex + i].normal = value;
                        });
                }

                if (const auto* uv = primitive.findAttribute("TEXCOORD_0"); uv != primitive.attributes.end())
                {
                    fastgltf::iterateAccessorWithIndex<glm::vec2>(
                        casset, casset.accessors[uv->accessorIndex], [&](glm::vec2 value, std::size_t i) {
                            model.vertices[plan.base_vertex + i].texCoord = value;
                        });
                }

                // glTF TANGENT is a vec4 (xyz tangent + w handedness). When present, pack it directly;
                // otherwise MikkTSpace fills these vertices after the parallel pass (needs the full
                // primitive at once). plan.has_tangent gates that.
                if (const auto* tan = primitive.findAttribute("TANGENT"); tan != primitive.attributes.end())
                {
                    plan.has_tangent = true;
                    fastgltf::iterateAccessorWithIndex<glm::vec4>(
                        casset, casset.accessors[tan->accessorIndex], [&](glm::vec4 value, std::size_t i) {
                            model.vertices[plan.base_vertex + i].tangent =
                                pack_tangent(glm::vec3(value), value.w);
                        });
                }

                if (primitive.indicesAccessor.has_value())
                {
                    const auto& index_accessor = casset.accessors[primitive.indicesAccessor.value()];
                    fastgltf::iterateAccessorWithIndex<uint32_t>(
                        casset, index_accessor, [&](uint32_t index, std::size_t i) {
                            model.indices[plan.index_offset + i] = plan.base_vertex + index;
                        });
                }
                else
                {
                    // Non-indexed primitive: synthesise a trivial index run.
                    for (uint32_t i = 0; i < plan.vertex_count; ++i)
                    {
                        model.indices[plan.index_offset + i] = plan.base_vertex + i;
                    }
                }

                plan.local_min = local_min;
                plan.local_max = local_max;
            }));
        }
        for (auto& job : jobs)
        {
            job.get();   // sync + rethrow any decode error
        }
    }

    // --- Tangents: generate via MikkTSpace for primitives the glTF didn't ship TANGENT for ------
    // MikkTSpace needs the whole primitive (it welds/averages across shared vertices), so it runs
    // after the parallel fill. Parallelised across primitives (each writes only its own vertex
    // range). Generates the standard per-vertex tangent basis the fragment shader expects.
    {
        std::vector<PrimitivePlan*> to_generate;
        for (PrimitivePlan& plan : plans)
        {
            if (!plan.has_tangent && plan.index_count > 0)
            {
                to_generate.push_back(&plan);
            }
        }
        if (!to_generate.empty())
        {
            string::core::job_system pool;
            std::vector<std::future<void>> jobs;
            jobs.reserve(to_generate.size());
            for (PrimitivePlan* plan : to_generate)
            {
                jobs.push_back(pool.enqueue([&model, plan]() {
                    MikktMesh mesh{
                        .verts = model.vertices.data() + plan->base_vertex,
                        .indices = model.indices.data() + plan->index_offset,
                        .base_vertex = plan->base_vertex,
                        .face_count = plan->index_count / 3,
                    };
                    SMikkTSpaceInterface iface{};
                    iface.m_getNumFaces = mikkt_num_faces;
                    iface.m_getNumVerticesOfFace = mikkt_num_verts_of_face;
                    iface.m_getPosition = mikkt_get_position;
                    iface.m_getNormal = mikkt_get_normal;
                    iface.m_getTexCoord = mikkt_get_texcoord;
                    iface.m_setTSpaceBasic = mikkt_set_tspace_basic;
                    SMikkTSpaceContext ctx{ &iface, &mesh };
                    genTangSpaceDefault(&ctx);
                }));
            }
            for (auto& job : jobs)
            {
                job.get();
            }
        }
        STRING_LOG_INFO("[load]   tangents: {} of {} primitives generated via MikkTSpace (rest from glTF TANGENT)",
                        to_generate.size(), plans.size());
    }

    // Regroup the filled primitives by mesh (preserving order) for the instance pass below.
    std::vector<std::vector<MeshPrimitive>> mesh_primitives(asset.meshes.size());
    for (const PrimitivePlan& plan : plans)
    {
        const auto& primitive = asset.meshes[plan.mesh_index].primitives[plan.primitive_index];
        const int32_t material = primitive.materialIndex.has_value()
            ? static_cast<int32_t>(primitive.materialIndex.value())
            : -1;
        mesh_primitives[plan.mesh_index].push_back(
            { plan.index_offset, plan.index_count, material, plan.local_min, plan.local_max });
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

}  // namespace string::asset::tools
