#pragma once

// Brief 09: display output transform + HDR color grading, baked into one 3D LUT at init.
//
// The composite pays ONE LUT sample per pixel; everything here runs on the CPU at bake time
// (init + on grading-parameter change). Two curves are bakeable behind r.tonemap:
//   - "aces2": an ACES-2.0-style CAM DRT — Hellwig2022 JMh colour appearance model, the ACES 2.0
//     Daniele-Evo tonescale (100-nit SDR parameterization), hue-preserving chroma compression and
//     a bisection gamut mapper to Rec.709. Structurally the ACES 2.0 output transform (hue skews
//     of the 1.x fitted curve are gone by construction: tonescale + compression operate in JMh,
//     never per-RGB-channel). NOT yet bit-matched against the official ACES 2.0 CTL reference —
//     flagged in the brief's running log as an open follow-up for the style checkpoint.
//   - "aces1": the existing Narkowicz ACES fitted curve (per-channel), kept for A/B.
//
// LUT input-domain encoding (the shaper): the LUT is indexed by a log2 encoding of the EXPOSED
// scene value: enc = (log2(max(c, 2^kLog2Min)) - kLog2Min) / (kLog2Max - kLog2Min), per channel.
// Post-exposure middle grey is 0.18, so [2^-12, 2^7] covers ~5e-5 .. 128 — deep shadow to the
// clamped sun disc — with the resolution concentrated perceptually (a linear-domain LUT bands
// in the darks). The same constants are hardcoded in composite.slang; keep them in lockstep.

#include <cstdint>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

namespace string::core::tonemap
{

inline constexpr float kLog2Min = -12.0f;
inline constexpr float kLog2Max = 7.0f;

enum class Curve : uint8_t { Aces2, Aces1 };

// HDR working-space grading ops, applied BEFORE the output transform when baking (composite still
// pays one LUT sample). CVar-driven v1; neutral defaults. Lift/gamma/gain are scalar v1 (per-
// channel is an artist-LUT-import follow-up) and operate on the shaper-encoded (log) value so
// they stay range-sane over the HDR domain.
struct Grading
{
    float exposure_stops = 0.0f;   // working-space exposure trim (post auto-exposure)
    float contrast = 1.0f;         // log-space contrast around 0.18 pivot
    float saturation = 1.0f;       // Rec.709-luma saturation
    float temperature = 0.0f;      // white balance, [-1, 1]; + warms
    float tint = 0.0f;             // white balance, [-1, 1]; + shifts green->magenta
    float lift = 0.0f;             // shaper-space lift (shadows)
    float gamma = 1.0f;            // shaper-space gamma (midtones)
    float gain = 1.0f;             // shaper-space gain (highlights)

    bool operator==(const Grading&) const = default;
    bool neutral() const { return *this == Grading{}; }
};

// The full transform for one exposed working-space value (linear Rec.709, middle grey 0.18):
// grading ops, then the selected output transform. Returns display-linear Rec.709 in [0, 1]
// (the swapchain's sRGB format does the encode). This IS the function the LUT tabulates; the
// capture writer calls the LUT-sampled version so captures match the screen bytes.
glm::vec3 transform(Curve curve, const Grading& grading, glm::vec3 exposed);

// Bake the 3D LUT: size^3 RGB texels (returned as tightly packed floats, rgb, r-fastest /
// b-slowest — laid out for a 2D strip of `size` slices side by side, width size*size, height
// size). Input domain is the shaper encoding above.
std::vector<float> bake_lut(Curve curve, const Grading& grading, uint32_t size);

// Shaper encode/decode + CPU trilinear sample of a baked LUT (capture-writer path; must match
// the GPU's normalized-coordinate filtering of the 2D strip).
glm::vec3 shaper_encode(glm::vec3 exposed);
glm::vec3 sample_lut(const std::vector<float>& lut, uint32_t size, glm::vec3 exposed);

}  // namespace string::core::tonemap
