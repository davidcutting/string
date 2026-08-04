#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace string
{

// One glyph's placement in the atlas and its layout metrics. All spatial values are in pixels at
// the atlas's baked `pixel_height`; UVs are normalised [0,1] into the atlas. A glyph is drawn by
// placing a quad of size (w,h) at (pen_x + xoff, pen_y + yoff) where pen_y is the baseline, then
// advancing the pen by `xadvance`.
struct glyph_metrics
{
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;  // atlas UVs
    float xoff = 0, yoff = 0;              // quad top-left relative to the pen origin (baseline)
    float xadvance = 0;                    // pen advance
    std::uint16_t w = 0, h = 0;            // glyph bitmap size (includes SDF padding)
};

// ASCII printable range [32, 126] — the only glyphs baked in v1. Space (32) has no bitmap but a
// real advance.
inline constexpr char kFirstGlyph = 32;
inline constexpr char kLastGlyph = 126;
inline constexpr std::size_t kGlyphCount = kLastGlyph - kFirstGlyph + 1;  // 95

// A single-channel (R8) signed-distance-field atlas for one font at one baked pixel size, plus the
// metrics needed to lay out and measure ASCII text. SDF is scale-independent: the shader samples
// the distance and thresholds it, so one atlas renders crisply at any on-screen size (see
// text.frag). Built on the CPU with stb_truetype; no GPU dependency, so it's unit-testable.
struct font_atlas
{
    std::vector<std::uint8_t> pixels;  // R8 signed distance, row-major, atlas_w * atlas_h
    std::uint32_t atlas_w = 0, atlas_h = 0;
    float pixel_height = 0;  // the size the SDF was baked at (glyph metrics are in these px)

    // Vertical metrics scaled to pixel_height. line_height() is the baseline-to-baseline advance.
    float ascent = 0, descent = 0, line_gap = 0;

    // SDF decode params for the shader: onedge is the normalised distance value that sits on the
    // glyph edge (coverage 0.5); pixel_dist is atlas distance-units per pixel (unused by the
    // fwidth-based shader AA, kept for reference / future fixed-width AA).
    float sdf_onedge = 0;
    float sdf_pixel_dist = 0;

    std::array<glyph_metrics, kGlyphCount> glyphs{};

    // Metrics for `c`, clamping out-of-range / unprintable characters to '?'.
    const glyph_metrics& glyph(char c) const;

    float line_height() const { return ascent - descent + line_gap; }
};

// Build an SDF atlas from a TrueType blob at the given pixel height. Rasterises each ASCII glyph to
// an SDF (stbtt_GetCodepointSDF) and rectangle-packs them into an atlas_w x atlas_h R8 image; if
// they don't fit, retries at double the height once. Throws std::runtime_error on a bad font or a
// pack that still overflows.
font_atlas build_font_atlas(std::span<const std::uint8_t> ttf, float pixel_height,
                            std::uint32_t atlas_w = 512, std::uint32_t atlas_h = 512);

}  // namespace string
