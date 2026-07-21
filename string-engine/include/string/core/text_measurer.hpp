#pragma once

#include <algorithm>
#include <cmath>
#include <span>

#include <string/core/font.hpp>
#include <string/core/layout.hpp>

namespace string
{

// A layout `measurer` (satisfies the concept in layout.hpp) that sizes a text element from a font
// atlas. It resolves the element's `text` handle against a builder's text table, sums glyph
// advances for the run's width, and uses the atlas line height × line count for the height —
// giving a FIT-sized text leaf a box that shrink-wraps its string. Non-text elements measure {0,0}.
//
// Pass it to layout_builder::end():
//   builder.end(available, text_measurer{ &atlas, builder.text_runs() });
//
// v1: left-to-right ASCII, no kerning (negligible for UI). font_px on the run is honoured only
// insofar as the atlas was baked at that size — a single-atlas SDF renderer scales at draw time.
struct text_measurer
{
    const font_atlas* atlas;
    std::span<const text_run> texts;

    dimension operator()(const element& e) const noexcept
    {
        if (e.text == 0 || atlas == nullptr || e.text >= texts.size())
        {
            return { 0, 0 };
        }
        const std::string_view str = texts[e.text].str;
        float width = 0, max_width = 0;
        int lines = 1;
        for (const char c : str)
        {
            if (c == '\n')
            {
                max_width = std::max(max_width, width);
                width = 0;
                ++lines;
                continue;
            }
            width += atlas->glyph(c).xadvance;
        }
        max_width = std::max(max_width, width);
        return { detail::to_u16(static_cast<int>(std::ceil(max_width))),
                 detail::to_u16(static_cast<int>(std::ceil(lines * atlas->line_height()))) };
    }
};

}  // namespace string
