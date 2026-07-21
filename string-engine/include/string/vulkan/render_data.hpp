#pragma once

#include <glm/glm.hpp>
#include <cstddef>
#include <vector>

#include <volk.h>

namespace String
{

struct Vertex {
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;
    glm::vec3 normal;   // glTF NORMAL; unused by the current shaders (kept for lighting)

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        return bindingDescription;
    }

    static std::vector<VkVertexInputAttributeDescription> getAttributeDescriptions() {
        std::vector<VkVertexInputAttributeDescription> attribute_descriptions = {
            { // Position
                .location = 0,
                .binding = 0,
                .format = VK_FORMAT_R32G32B32_SFLOAT,
                .offset = offsetof(Vertex, pos)
            },
            { // Color
                .location = 1,
                .binding = 0,
                .format = VK_FORMAT_R32G32B32_SFLOAT,
                .offset = offsetof(Vertex, color)
            },
            { // texture coords
                .location = 2,
                .binding = 0,
                .format = VK_FORMAT_R32G32_SFLOAT,
                .offset = offsetof(Vertex, texCoord)
            },
            { // normal
                .location = 3,
                .binding = 0,
                .format = VK_FORMAT_R32G32B32_SFLOAT,
                .offset = offsetof(Vertex, normal)
            }
        };

        return attribute_descriptions;
    }

    bool operator==(const Vertex& other) const {
        return pos == other.pos && color == other.color &&
               texCoord == other.texCoord && normal == other.normal;
    }
};

// The 3D vertex shader pulls vertices from a device-address SSBO using GL_EXT_scalar_block_layout,
// which packs its Vertex struct to match this tightly-packed layout exactly. If these change, the
// shader's `Vertex` (and any assumed stride) must change with them — a mismatch is silent garbage.
static_assert(sizeof(Vertex) == 44, "Vertex must stay tightly packed for scalar-layout pulling");
static_assert(offsetof(Vertex, pos) == 0);
static_assert(offsetof(Vertex, color) == 12);
static_assert(offsetof(Vertex, texCoord) == 24);
static_assert(offsetof(Vertex, normal) == 32);

}  // namespace String

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

namespace std {
template <>
struct hash<String::Vertex> {
    size_t operator()(String::Vertex const& vertex) const {
        return (((hash<glm::vec3>()(vertex.pos) ^ (hash<glm::vec3>()(vertex.color) << 1)) >> 1) ^
                (hash<glm::vec2>()(vertex.texCoord) << 1)) ^
               (hash<glm::vec3>()(vertex.normal) << 1);
    }
};
}  // namespace std