#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <vector>

#include <string/ui/font.hpp>

using namespace string;

namespace
{
std::vector<std::uint8_t> read_font()
{
    std::ifstream f(STRING_FONT_TEST_PATH, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}
}  // namespace

TEST(Font, BuildsAtlasFromTtf)
{
    const std::vector<std::uint8_t> ttf = read_font();
    ASSERT_FALSE(ttf.empty()) << "test font not found at " << STRING_FONT_TEST_PATH;

    const font_atlas atlas = build_font_atlas(ttf, 32.0f);

    // Atlas image is populated.
    EXPECT_GT(atlas.atlas_w, 0u);
    EXPECT_GT(atlas.atlas_h, 0u);
    EXPECT_EQ(atlas.pixels.size(), static_cast<std::size_t>(atlas.atlas_w) * atlas.atlas_h);

    // It has non-zero coverage somewhere (glyphs actually rasterised, not a blank atlas).
    const bool any_coverage =
        std::any_of(atlas.pixels.begin(), atlas.pixels.end(), [](std::uint8_t p) { return p != 0; });
    EXPECT_TRUE(any_coverage);

    // Vertical metrics are sane: ascent above the baseline, descent below, positive line height.
    EXPECT_GT(atlas.ascent, 0.0f);
    EXPECT_LT(atlas.descent, 0.0f);
    EXPECT_GT(atlas.line_height(), 0.0f);
}

TEST(Font, GlyphMetrics)
{
    const std::vector<std::uint8_t> ttf = read_font();
    ASSERT_FALSE(ttf.empty());
    const font_atlas atlas = build_font_atlas(ttf, 32.0f);

    // A visible letter advances the pen and has a non-empty bitmap with a valid UV rect.
    const glyph_metrics& a = atlas.glyph('A');
    EXPECT_GT(a.xadvance, 0.0f);
    EXPECT_GT(a.w, 0u);
    EXPECT_GT(a.h, 0u);
    EXPECT_LT(a.u0, a.u1);
    EXPECT_LT(a.v0, a.v1);

    // Space advances but has no bitmap.
    const glyph_metrics& space = atlas.glyph(' ');
    EXPECT_GT(space.xadvance, 0.0f);
    EXPECT_EQ(space.w, 0u);

    // Out-of-range characters fall back to '?' rather than reading out of bounds.
    EXPECT_EQ(&atlas.glyph('\x01'), &atlas.glyph('?'));
    EXPECT_EQ(&atlas.glyph(static_cast<char>(200)), &atlas.glyph('?'));
}
