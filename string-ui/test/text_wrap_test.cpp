#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

#include <string/ui/text_measurer.hpp>

using namespace string;

namespace
{
std::vector<std::uint8_t> read_font()
{
    std::ifstream f(STRING_FONT_TEST_PATH, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

constexpr std::string_view kParagraph =
    "Drag the right edge and this paragraph re-flows: it takes whatever width the layout "
    "gives it and asks for the height its lines need.";
}  // namespace

// THE REGRESSION TEST FOR THE ONE DEFECT THIS FEATURE SHIPPED WITH (user, 2026-08-01: "a few panel
// size positions where a word gets cut off instead of wrapping").
//
// The break decision used to be made per GLYPH — "does the next character still fit" — which is
// character wrap, not word wrap. It looks correct at most widths and fails at exactly those widths
// where a word's first letter fits and its tail does not: the tail runs past the box and is clipped.
//
// So the invariant is swept across widths rather than checked at one: a single width would have
// passed on the buggy code. NO LINE MAY EVER BE WIDER THAN THE WRAP WIDTH.
TEST(TextWrap, NoLineExceedsTheWrapWidthAtAnyWidth)
{
    const std::vector<std::uint8_t> ttf = read_font();
    ASSERT_FALSE(ttf.empty()) << "test font not found at " << STRING_FONT_TEST_PATH;
    dynamic_font_atlas atlas(ttf);

    // element.text is 1-based against this table; index 0 is the "no text" sentinel.
    const std::vector<text_run> texts{ text_run{}, text_run{ kParagraph, 16 } };
    dynamic_text_measurer m{ &atlas, texts };

    element e{};
    e.text = 1;
    e.wrap = true;
    const float s = m.scale_of(texts[1]);
    const int floor_px = m.min_width(e);
    ASSERT_GT(floor_px, 0);

    bool saw_a_wrap = false;
    for (int w = floor_px; w <= 600; ++w)
    {
        const auto sh = m.shape(texts[1], s, static_cast<float>(w));
        EXPECT_LE(sh.width, static_cast<float>(w))
            << "a line overflowed at wrap width " << w << " — the tail of a word escaped its box";
        if (sh.lines > 1) saw_a_wrap = true;
    }
    EXPECT_TRUE(saw_a_wrap) << "the sweep must actually exercise wrapping";
}

// The floor the layout applies during shrink has to be a width the text can really be laid out at.
TEST(TextWrap, MinWidthIsTheWidestWord)
{
    const std::vector<std::uint8_t> ttf = read_font();
    ASSERT_FALSE(ttf.empty());
    dynamic_font_atlas atlas(ttf);

    const std::vector<text_run> texts{ text_run{}, text_run{ kParagraph, 16 } };
    dynamic_text_measurer m{ &atlas, texts };
    element e{};
    e.text = 1;
    e.wrap = true;

    const int floor_px = m.min_width(e);
    const float s = m.scale_of(texts[1]);

    // At exactly the floor, every word fits on its own line, so nothing is hard-broken mid-word:
    // the widest line is the widest word and no line overflows.
    const auto at_floor = m.shape(texts[1], s, static_cast<float>(floor_px));
    EXPECT_LE(at_floor.width, static_cast<float>(floor_px));
    EXPECT_GT(at_floor.lines, 1);

    // Unwrapped, the whole run is one line and is wider than any single word.
    const auto unwrapped = m.shape(texts[1], s, 0.0f);
    EXPECT_EQ(unwrapped.lines, 1);
    EXPECT_GT(unwrapped.width, static_cast<float>(floor_px));
}

// Wrapping wider always needs no more lines than wrapping narrower. A break rule that looked at one
// glyph could violate this; a whole-word rule cannot.
TEST(TextWrap, LineCountIsMonotoneInWidth)
{
    const std::vector<std::uint8_t> ttf = read_font();
    ASSERT_FALSE(ttf.empty());
    dynamic_font_atlas atlas(ttf);

    const std::vector<text_run> texts{ text_run{}, text_run{ kParagraph, 16 } };
    dynamic_text_measurer m{ &atlas, texts };
    element e{};
    e.text = 1;
    e.wrap = true;
    const float s = m.scale_of(texts[1]);

    int previous = 1 << 30;
    for (int w = m.min_width(e); w <= 600; ++w)
    {
        const int lines = m.shape(texts[1], s, static_cast<float>(w)).lines;
        EXPECT_LE(lines, previous) << "widening the box added a line, at width " << w;
        previous = lines;
    }
}

// A word longer than the whole line still has to be broken rather than allowed to run out of the
// box — the one case where breaking mid-word is correct.
TEST(TextWrap, AnOverlongWordIsHardBroken)
{
    const std::vector<std::uint8_t> ttf = read_font();
    ASSERT_FALSE(ttf.empty());
    dynamic_font_atlas atlas(ttf);

    const std::vector<text_run> texts{ text_run{},
                                       text_run{ "supercalifragilisticexpialidocious", 16 } };
    dynamic_text_measurer m{ &atlas, texts };
    const float s = m.scale_of(texts[1]);

    const auto sh = m.shape(texts[1], s, 40.0f);
    EXPECT_GT(sh.lines, 1) << "an unbreakable word must still be broken, not overflowed";
    EXPECT_LE(sh.width, 40.0f);
}

// --- Measure and place are one algorithm ---------------------------------------------------------
//
// THE invariant the duplication risked. The layout reserves height for N lines; the renderer walks
// the same string and puts glyphs on however many lines ITS copy of the wrap rules produced. When
// those two numbers disagree, text overdraws whatever the layout placed below it — and two hand-kept
// implementations can agree for years and then differ on one string at one width.
//
// So: no glyph may land on a line the measurement did not reserve. Swept across widths for the same
// reason the wrap test is — a single width proves almost nothing.
//
// (Deliberately NOT asserted: that glyph QUADS fit the measured box. A measured box comes from
// advances and line heights, while a quad is the atlas bitmap — which for SDF glyphs carries spread
// padding on every side, so it legitimately spills a pixel or two, exactly as overhangs and italic
// tails do in any text stack. Asserting that would pin a bug, not a property.)
TEST(TextWrap, PlacementNeverUsesMoreLinesThanMeasurementReserved)
{
    const std::vector<std::uint8_t> ttf = read_font();
    ASSERT_FALSE(ttf.empty()) << "test font not found at " << STRING_FONT_TEST_PATH;
    dynamic_font_atlas atlas(ttf);

    const std::vector<text_run> texts{ text_run{}, text_run{ kParagraph, 16 } };
    dynamic_text_measurer m{ &atlas, texts };

    element e{};
    e.text = 1;
    e.wrap = true;

    for (std::uint16_t w = 60; w <= 420; ++w)
    {
        e.sizing.width = fixed(w);
        // What LAYOUT reserves: height_at is the call the wrap seam makes mid-layout.
        const std::uint16_t box_h = m.height_at(e, w);
        const int reserved_lines =
            static_cast<int>(std::lround(box_h / (atlas.line_height() * m.scale_of(texts[1]))));

        // What the RENDERER draws — the same call, with the glyphs collected this time.
        int max_line = 0;
        std::size_t drawn = 0;
        const shaped_text sh = shape_text(atlas, kParagraph, m.scale_of(texts[1]),
                                          static_cast<float>(w),
                                          [&](const placed_glyph& g) {
                                              max_line = std::max(max_line, g.line);
                                              ++drawn;
                                          });

        EXPECT_GT(drawn, 0u);
        EXPECT_EQ(sh.lines, reserved_lines) << "measured line count disagrees at width " << w;
        EXPECT_LT(max_line, reserved_lines) << "glyph on an unreserved line at width " << w;
    }
}

// The metrics a caller gets must not depend on whether it asked for glyphs — otherwise "measure"
// and "draw" are still two behaviours wearing one name.
TEST(TextWrap, ShapingWithAndWithoutAGlyphSinkAgree)
{
    const std::vector<std::uint8_t> ttf = read_font();
    ASSERT_FALSE(ttf.empty());
    dynamic_font_atlas atlas(ttf);

    for (std::uint16_t w : { 0, 40, 137, 300 })
    {
        std::size_t drawn = 0;
        const shaped_text measured =
            shape_text(atlas, kParagraph, 0.4f, static_cast<float>(w), no_glyphs{});
        const shaped_text placed =
            shape_text(atlas, kParagraph, 0.4f, static_cast<float>(w),
                       [&](const placed_glyph&) { ++drawn; });

        EXPECT_EQ(measured.lines, placed.lines) << "at wrap width " << w;
        EXPECT_FLOAT_EQ(measured.width, placed.width) << "at wrap width " << w;
        EXPECT_GT(drawn, 0u);
    }
}

// --- The shaping cache (brief 12b M0) ------------------------------------------------------------
//
// The whole point is that a hit must be INDISTINGUISHABLE from a fresh shape: this is a pure
// optimization, so every test here is really asking "does caching change the answer".

TEST(ShapeCache, AHitReturnsExactlyWhatShapingWouldHave)
{
    const std::vector<std::uint8_t> ttf = read_font();
    ASSERT_FALSE(ttf.empty());
    dynamic_font_atlas atlas(ttf);
    text_shape_cache cache;

    std::vector<placed_glyph> direct;
    const shaped_text expect = shape_text(atlas, kParagraph, 0.4f, 300.0f,
                                          [&](const placed_glyph& g) { direct.push_back(g); });

    for (int i = 0; i < 3; ++i)   // miss, then hits
    {
        const auto& run = cache.get(atlas, kParagraph, 0.4f, 300.0f);
        EXPECT_FLOAT_EQ(run.metrics.width, expect.width);
        EXPECT_EQ(run.metrics.lines, expect.lines);
        ASSERT_EQ(run.glyphs.size(), direct.size());
        for (std::size_t g = 0; g < direct.size(); ++g)
        {
            EXPECT_FLOAT_EQ(run.glyphs[g].x, direct[g].x);
            EXPECT_FLOAT_EQ(run.glyphs[g].y, direct[g].y);
            EXPECT_EQ(run.glyphs[g].line, direct[g].line);
            EXPECT_EQ(run.glyphs[g].metrics, direct[g].metrics);
        }
    }
    EXPECT_EQ(cache.misses(), 1u);
    EXPECT_EQ(cache.hits(), 2u);
}

// Wrap width and scale are part of the ANSWER, so they must be part of the key — sharing an entry
// across them would hand a caller a run wrapped to somebody else's box.
TEST(ShapeCache, KeysOnWrapWidthAndScaleNotJustContent)
{
    const std::vector<std::uint8_t> ttf = read_font();
    ASSERT_FALSE(ttf.empty());
    dynamic_font_atlas atlas(ttf);
    text_shape_cache cache;

    const int narrow = cache.get(atlas, kParagraph, 0.4f, 120.0f).metrics.lines;
    const int wide = cache.get(atlas, kParagraph, 0.4f, 600.0f).metrics.lines;
    EXPECT_GT(narrow, wide) << "different wrap widths must not share an entry";

    const float small = cache.get(atlas, kParagraph, 0.2f, 300.0f).metrics.width;
    const float large = cache.get(atlas, kParagraph, 0.8f, 300.0f).metrics.width;
    EXPECT_NE(small, large) << "different scales must not share an entry";
    EXPECT_EQ(cache.hits(), 0u);
    EXPECT_EQ(cache.misses(), 4u);
}

// Frame-aging, the same rule Motion uses: a run nobody asked for this frame is dropped, so a
// scrolled-away table does not pin its rows forever.
TEST(ShapeCache, EvictsEntriesNotTouchedThisFrame)
{
    const std::vector<std::uint8_t> ttf = read_font();
    ASSERT_FALSE(ttf.empty());
    dynamic_font_atlas atlas(ttf);
    text_shape_cache cache;

    cache.get(atlas, "alpha", 0.4f, 0.0f);
    cache.get(atlas, "beta", 0.4f, 0.0f);
    EXPECT_EQ(cache.size(), 2u);

    cache.end_frame();              // both were touched this frame: both survive
    EXPECT_EQ(cache.size(), 2u);

    cache.get(atlas, "alpha", 0.4f, 0.0f);   // only alpha is asked for
    cache.end_frame();
    EXPECT_EQ(cache.size(), 1u);
}

// Atlas growth must NOT invalidate: glyphs bake once at bake_px and their metrics are stable, so a
// new codepoint only ADDS entries. If this ever fails, something started mutating baked metrics and
// the cache key needs an epoch.
TEST(ShapeCache, NewGlyphsInTheAtlasDoNotInvalidateExistingRuns)
{
    const std::vector<std::uint8_t> ttf = read_font();
    ASSERT_FALSE(ttf.empty());
    dynamic_font_atlas atlas(ttf);
    text_shape_cache cache;

    const shaped_text before = cache.get(atlas, "hello", 0.4f, 0.0f).metrics;
    cache.get(atlas, "\xE4\xB8\x96\xE7\x95\x8C", 0.4f, 0.0f);   // CJK: rasterises new glyphs
    const shaped_text after = cache.get(atlas, "hello", 0.4f, 0.0f).metrics;

    EXPECT_FLOAT_EQ(before.width, after.width);
    EXPECT_EQ(before.lines, after.lines);
}
