#pragma once

#include <cstddef>
#include <cstdint>

#include <volk.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

// Brief 09b — relightable irradiance probe volume (DDGI-style, raster capture v1).
//
// A uniform grid of probes fit to the scene AABB. Each probe stores, in octahedral atlases:
//   - IRRADIANCE (8x8 per probe, RGB16F): cosine-convolved incoming radiance, relit from the live
//     sun/sky every time the sun moves (the dynamic part). This replaces the sky-SH ambient diffuse
//     term in lighting.slang, gated by Chebyshev visibility so outdoor probes don't leak indoors.
//   - VISIBILITY (16x16 per probe, RG16F: mean distance, mean distance^2): DDGI variance-shadow
//     statistics of the STATIC scene, captured once at load, never relit. The Chebyshev test on
//     this is what keeps the technique leak-free.
//   - CAPTURE G-BUFFER (16x16 per probe): static per-direction {albedo, world normal, hit distance |
//     sky-miss}, rasterized once at load. The relight compute reads it to recompute radiance under
//     the current sun (miss -> sky_env; hit -> albedo * (sun_direct + bounce)).
//
// Each octahedral tile carries a 1-texel gutter (DDGI border) replicated from the opposite edge so
// hardware bilinear filtering across the tile wraps correctly. So an 8x8 irradiance probe occupies
// a 10x10 footprint; a 16x16 visibility/capture probe occupies 18x18.
//
// Atlas layout: probes are tiled 2D. The X axis packs (probes_x * probes_y) tiles across; the Y axis
// packs probes_z tiles down. i.e. atlas_tile(px,py,pz) = column (pz? no) — see probe_common.slang
// probe_tile_origin for the exact addressing. Addressing is centralized in probe_common.slang so a
// future clipmap-scrolled cascade layout (Phase B) is a data-layout change there, not a rewrite.
//
// Phase 2 (NOT built): the capture G-buffer is replaced by VK_KHR_ray_query per-frame probe rays;
// the atlases, relight, sampling and debug all stay. Keep capture texel production isolated.
namespace sandbox
{

// Per-probe octahedral tile inner sizes (excluding the 1-texel gutter on each side).
inline constexpr uint32_t kProbeIrradSize = 8;      // 8x8 irradiance octahedron
inline constexpr uint32_t kProbeVisSize = 16;       // 16x16 visibility / capture octahedron
inline constexpr uint32_t kProbeIrradStride = kProbeIrradSize + 2;   // + 1-texel gutter each side
inline constexpr uint32_t kProbeVisStride = kProbeVisSize + 2;

// Max probes along the longest axis (clamps the grid so a huge scene AABB can't blow up the atlas).
inline constexpr uint32_t kProbeMaxPerAxis = 32;
// Hard cap on total probes (atlas VRAM + relight cost guard). 16*16*16 = 4096 typical for Sponza.
inline constexpr uint32_t kProbeMaxTotal = 32 * 1024;

// The uniform probe grid fit to a scene AABB. Origin is the world position of probe (0,0,0); probes
// are placed at origin + (i,j,k) * spacing. counts are per-axis probe counts (>=2). Held CPU-side by
// GeometryPass and mirrored into the shader push/scene-data so sampling agrees exactly.
struct ProbeVolume
{
    glm::vec3 origin{ 0.0f };     // world pos of probe index (0,0,0)
    glm::vec3 spacing{ 1.0f };    // world units between adjacent probes per axis
    glm::uvec3 counts{ 0 };       // probe counts per axis
    bool valid = false;

    uint32_t total() const { return counts.x * counts.y * counts.z; }
    // Atlas tile grid: (counts.x * counts.y) columns of tiles, counts.z rows.
    glm::uvec2 tile_grid() const { return { counts.x * counts.y, counts.z }; }
};

// GPU push/scene-data mirror of the volume (std430/natural friendly: vec4s so alignment is trivial).
// Consumed by probe_common.slang (addressing) — MUST match ProbeGrid there field for field.
struct GpuProbeGrid
{
    glm::vec4 origin_spacing_x;   // xyz origin, w spacing.x
    glm::vec4 counts_spacing_yz;  // xyz counts (as float), then spacing.y in... no: see below
    // Kept explicit to avoid packing ambiguity: origin.xyz + spacing.xyz + counts.xyz + atlas dims.
};

// Push constant for the probe relight + convolve compute (matches Push in probe_relight.slang).
struct ProbeRelightPush
{
    glm::vec4 origin_spacing;   // xyz grid origin, w = spacing.x (assume uniform-ish; per-axis below)
    glm::vec4 spacing;          // xyz per-axis spacing, w unused
    glm::uvec4 counts;          // xyz probe counts, w = total
    glm::vec4 sun_dir;          // xyz dir TO sun, w = sun_intensity (klx)
    glm::vec4 sun_color;        // xyz, w = hysteresis (0..1 blend toward new)
    glm::vec4 sky_zenith;       // xyz, w unused
    glm::vec4 sky_ground;       // xyz ground albedo, w unused
    uint32_t cap_gbuf_slot;     // capture G-buffer (normal+dist) sampled slot
    uint32_t cap_albedo_slot;   // capture albedo sampled slot
    uint32_t irrad_prev_slot;   // previous irradiance atlas sampled slot (bounce + hysteresis src)
    uint32_t irrad_dst_slot;    // irradiance atlas storage slot (write)
    uint32_t vis_slot;          // visibility atlas sampled slot (for bounce Chebyshev)
    uint32_t first_frame;       // 1 -> no hysteresis (initialize)
    uint32_t probe_base;        // first probe of this dispatch (amortized)
    uint32_t probe_count;       // probes in this dispatch
    VkDeviceAddress sh;         // sky SH buffer (bounce fallback where the probe weight is ~0)
    VkDeviceAddress active;     // per-probe activation state (1 active / 0 inside geometry); relight skips 0
    VkDeviceAddress offsets;    // per-probe relocation offsets (hit points + bounce use relocated pos)
    VkDeviceAddress scene;      // this frame's SceneData (CSM cascades + view for the 1-tap sun shadow)
};
static_assert(offsetof(ProbeRelightPush, sh) == 144);
static_assert(offsetof(ProbeRelightPush, active) == 152);
static_assert(offsetof(ProbeRelightPush, offsets) == 160);
static_assert(offsetof(ProbeRelightPush, scene) == 168);
static_assert(sizeof(ProbeRelightPush) == 176);

// Push constant for the instanced probe-debug sphere draw (matches Push in probe_debug.slang).
struct ProbeDebugPush
{
    glm::mat4 view_proj;        // 0
    glm::vec4 origin_spacing;   // 64  xyz origin, w = sphere radius
    glm::vec4 spacing;          // 80  xyz per-axis spacing, w unused
    glm::uvec4 counts;          // 96  xyz probe counts, w = mode (0 irradiance, 1 visibility)
    glm::vec4 camera_pos;       // 112 xyz, w = exposure scale (match composite EV so HDR reads sanely)
    uint32_t irrad_slot;        // 128 irradiance atlas sampled slot
    uint32_t vis_slot;          // 132 visibility atlas sampled slot
    uint32_t _pad0;             // 136
    uint32_t _pad1;             // 140
    VkDeviceAddress active;     // 144 per-probe activation (inactive probes' spheres are hidden)
    VkDeviceAddress offset;     // 152 per-probe relocation offset (moves sphere out of walls)
};
static_assert(offsetof(ProbeDebugPush, active) == 144);
static_assert(offsetof(ProbeDebugPush, offset) == 152);

}  // namespace sandbox
