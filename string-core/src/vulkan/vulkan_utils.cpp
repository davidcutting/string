#include <string>
#include <unordered_map>
#include <string/vulkan/render_data.hpp>
#include <string/vulkan/vulkan_utils.hpp>

#define TINYOBJLOADER_IMPLEMENTATION
#include <string/core/tiny_obj_loader.h>

namespace string
{
namespace vku
{

void load_model(const std::filesystem::path& model_path, std::vector<Vertex>& vertex_buffer, std::vector<uint32_t>& index_buffer)
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, model_path.string().c_str()))
    {
        throw std::runtime_error(warn + err);
    }

    std::unordered_map<Vertex, uint32_t> unique_vertices{};

    for (const auto& shape : shapes)
    {
        for (const auto& index : shape.mesh.indices)
        {
            Vertex vertex{};

            vertex.pos = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2]
            };

            vertex.texCoord = {
                attrib.texcoords[2 * index.texcoord_index + 0],
                1.0f - attrib.texcoords[2 * index.texcoord_index + 1]
            };

            vertex.color = {1.0f, 1.0f, 1.0f};

            if (unique_vertices.count(vertex) == 0)
            {
                unique_vertices[vertex] = static_cast<uint32_t>(vertex_buffer.size());
                vertex_buffer.push_back(vertex);
            }

            index_buffer.push_back(unique_vertices[vertex]);
        }
    }
}

}

}