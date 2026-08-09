#pragma once

#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>

namespace string
{

struct Vertex {
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;
    glm::vec3 normal;   // glTF NORMAL
    // Packed vertex tangent + handedness for normal mapping, filled from the glTF TANGENT attribute
    // or generated via MikkTSpace at load (see gltf_loader). Bit layout 10:10:10:2 — xyz are each a
    // 10-bit snorm of the normalized tangent, w is the 2-bit sign (0 -> -1, else +1). Unpacked in
    // meshlet_mesh.slang — a mismatch between this packing and the shader is silent garbage.
    uint32_t tangent = 0;
};

// The 3D vertex shader pulls vertices from a device-address SSBO using GL_EXT_scalar_block_layout,
// which packs its Vertex struct to match this tightly-packed layout exactly. If these change, the
// shader's `Vertex` (and any assumed stride) must change with them — a mismatch is silent garbage.
static_assert(sizeof(Vertex) == 48, "Vertex must stay tightly packed for scalar-layout pulling");
static_assert(offsetof(Vertex, pos) == 0);
static_assert(offsetof(Vertex, color) == 12);
static_assert(offsetof(Vertex, texCoord) == 24);
static_assert(offsetof(Vertex, normal) == 32);
static_assert(offsetof(Vertex, tangent) == 44);

}  // namespace string