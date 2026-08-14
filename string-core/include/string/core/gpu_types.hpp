#pragma once

#include <cstdint>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

namespace string
{

// A dynamic local light (point or spot). std430-friendly (4x vec4 = 64B). Matches GpuLight in
// lighting.slang and froxel_cull.slang. Lives in string-core (not the renderer) because it is the
// GPU layout of a SCENE object: the scene layer describes lights, every renderer consumes them.
enum class LightType : uint32_t { POINT = 0, SPOT = 1 };

struct GpuLight
{
    glm::vec4 position_radius;   // xyz world position, w = range (radius) for attenuation
    glm::vec4 color_intensity;   // rgb linear color, w = intensity (radiant scale)
    glm::vec4 direction_type;    // xyz spot direction (normalized), w = LightType as float
    glm::vec4 cone;              // x = cos(inner), y = cos(outer); zw unused (point ignores all)
};
static_assert(sizeof(GpuLight) == 64);

}  // namespace string
