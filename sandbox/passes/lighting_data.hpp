#pragma once

#include <cstddef>
#include <cstdint>

#include <volk.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

namespace sandbox
{

// Number of stabilized shadow cascades. Quality-tier CVar from day one (see LightingSettings): the
// shader loops MAX_CASCADES and the CPU fills only `cascade_count` of them. Bump both together.
inline constexpr uint32_t kMaxCascades = 4;

// Froxel (clustered Forward+) grid. Screen tiles x depth slices; log-depth distribution. These are
// the CVar-able froxel dimensions from the brief — the compute binning and the fragment lookup both
// derive their grid from SceneData's froxel fields, which are filled from LightingSettings.
inline constexpr uint32_t kFroxelTileSize = 16;    // pixels per tile edge
inline constexpr uint32_t kFroxelDepthSlices = 24; // log-depth slices
// Max lights indexed per froxel. Overflow is dropped (heatmap makes gross over-binning visible).
inline constexpr uint32_t kMaxLightsPerFroxel = 128;

// A dynamic local light (point or spot). std430-friendly (4x vec4 = 64B). Matches GpuLight in
// lighting.slang and froxel_cull.slang.
enum class LightType : uint32_t { POINT = 0, SPOT = 1 };

struct GpuLight
{
    glm::vec4 position_radius;   // xyz world position, w = range (radius) for attenuation
    glm::vec4 color_intensity;   // rgb linear color, w = intensity (radiant scale)
    glm::vec4 direction_type;    // xyz spot direction (normalized), w = LightType as float
    glm::vec4 cone;              // x = cos(inner), y = cos(outer); zw unused (point ignores all)
};
static_assert(sizeof(GpuLight) == 64);

// Per-frame scene lighting/shadow/froxel constants, bound as an SSBO (persistent-mapped ring) and
// reached by device address from the meshlet push. Moved out of the push constant because the CSM +
// froxel data overflow the 128/256B push budget. Field order/padding must match SceneData in
// lighting.slang (std430 scalars; vec3s padded to 16B).
struct SceneData
{
    glm::vec3 camera_pos;   float _pad0;
    glm::vec3 sun_dir;      float sun_intensity;   // direction TO the sun (normalized)
    glm::vec3 sun_color;    float _pad1;
    glm::vec3 ambient_sky;  float _pad2;           // hemispheric ambient, up
    glm::vec3 ambient_ground; float _pad3;         // hemispheric ambient, down

    // Cascaded shadow maps. Per cascade: light view-projection, the view-space split far distance,
    // world units per shadow texel (for the normal-offset bias), and the bindless shadow-map slot.
    glm::mat4 cascade_view_proj[kMaxCascades];
    glm::vec4 cascade_split[kMaxCascades];      // .x = view-space far distance of this cascade
    glm::vec4 cascade_texel[kMaxCascades];      // .x = world units per texel (bias scale)
    glm::uvec4 cascade_slot[kMaxCascades];      // .x = bindless slot of this cascade's depth image
    uint32_t cascade_count;
    float shadow_texel;             // 1/resolution (PCF step in shadow UV space)
    float shadow_bias;              // constant depth bias (reverse-Z units)
    float shadow_normal_offset_scale;  // multiplier on per-cascade world texel size
    float cascade_blend;            // fraction of a cascade's far distance to cross-fade over

    // NO padding before `view`: Slang's NATURAL layout (what pointer-loaded structs use) packs
    // float4x4 with scalar (4-byte) alignment, so `view` follows cascade_blend directly at 548.
    // Verified against the SPIR-V OpMemberDecorate offsets of %SceneData_natural — reflection of
    // the std430 variant lies about this struct.

    // Froxel grid: tile size, depth-slice count, grid extents (tiles x, tiles y, slices), near/far
    // for the log-depth mapping, and the device addresses of the light SSBO + froxel index SSBO.
    glm::mat4 view;                 // camera view matrix (fragment recovers view-space z for slicing)
    glm::uvec4 froxel_dims;         // x=tiles_x, y=tiles_y, z=slices, w=tile_size(px)
    glm::vec4 froxel_planes;        // x=near, y=far, z=1/log(far/near) precomputed, w=light_count
    VkDeviceAddress lights;         // device address of the GpuLight SSBO
    VkDeviceAddress froxels;        // device address of the per-froxel light-index SSBO
    uint32_t max_lights_per_froxel;
    uint32_t debug_flags;           // bit 0: froxel light-count heatmap; bit 1: furnace test (brief 07)
    uint32_t _tail0;
    uint32_t _tail1;

    // Brief 07 dynamic sky IBL. APPENDED so every pre-07 offset above is unchanged; must match
    // the tail of SceneData in lighting.slang (natural layout, pointer 8-aligned at 680).
    VkDeviceAddress sh;             // ShBuffer: 9 float4 L2 SH irradiance coefficients (E/pi)
    uint32_t env_slot;              // bindless SamplerCube slot of the prefiltered environment
    uint32_t env_mips;              // prefiltered ladder mip count
    uint32_t dfg_slot;              // bindless Sampler2D slot of the split-sum DFG LUT
    uint32_t _tail2;
};

// Lock the layout against Slang's NATURAL layout for pointer-loaded structs (scalar packing:
// float3 is 12B/4-aligned, float4x4 is 64B/4-aligned, pointers are 8-aligned). Offsets below are
// copied from the SPIR-V %SceneData_natural OpMemberDecorate list (slangc -target spirv-asm).
// A drift here is silent GPU garbage or a GPUVM fault.
static_assert(offsetof(SceneData, cascade_view_proj) == 80);
static_assert(offsetof(SceneData, cascade_split) == 336);
static_assert(offsetof(SceneData, cascade_texel) == 400);
static_assert(offsetof(SceneData, cascade_slot) == 464);
static_assert(offsetof(SceneData, cascade_count) == 528);
static_assert(offsetof(SceneData, view) == 548);
static_assert(offsetof(SceneData, froxel_dims) == 612);
static_assert(offsetof(SceneData, froxel_planes) == 628);
static_assert(offsetof(SceneData, lights) == 648);
static_assert(offsetof(SceneData, froxels) == 656);
static_assert(offsetof(SceneData, max_lights_per_froxel) == 664);
// Brief 07 tail (verified against %SceneData_natural OpMemberDecorate offsets, see the brief log).
static_assert(offsetof(SceneData, sh) == 680);
static_assert(offsetof(SceneData, env_slot) == 688);
static_assert(offsetof(SceneData, env_mips) == 692);
static_assert(offsetof(SceneData, dfg_slot) == 696);
static_assert(sizeof(SceneData) == 704);

}  // namespace sandbox
