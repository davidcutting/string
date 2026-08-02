#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

#include <string/core/text_measurer.hpp>

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
