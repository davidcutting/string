#pragma once

#include <algorithm>
#include <cmath>
#include <span>

#include <string/core/dynamic_font.hpp>
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

// A measurer over the dynamic (Unicode, grow-on-demand) atlas. Scales metrics by font_px / bake_px so
// a single reference-size SDF atlas renders text at any requested size, decodes UTF-8, applies
// kerning, honours explicit '\n', and word-wraps to `wrap_px` (0 = no wrap). Must match the shaper in
// ui_pass exactly so measured boxes fit the drawn glyphs. Non-const atlas: measuring caches glyphs.
struct dynamic_text_measurer
{
    dynamic_font_atlas* atlas;
    std::span<const text_run> texts;
    std::uint16_t wrap_px = 0;  // element cross-size wrap width; 0 = single unwrapped line(s)

    dimension operator()(const element& e) const
    {
        if (e.text == 0 || atlas == nullptr || e.text >= texts.size())
            return { 0, 0 };
        const text_run& run = texts[e.text];
        const float px = run.font_px > 0 ? static_cast<float>(run.font_px) : atlas->bake_px();
        const float s = px / atlas->bake_px();

        // Wrap width comes from the ELEMENT: a fixed-width text element word-wraps to exactly that
        // width (its box), everything else is unwrapped (single line, clipped by the box). This is
        // the contract the ui_pass shaper mirrors — the two MUST agree or wrapped text draws more
        // lines than the measured (1-line) box reserves and overdraws the rows below it.
        const std::uint16_t wrap = e.sizing.width.mode == size_mode::FIXED
                                       ? e.sizing.width.value : wrap_px;

        float width = 0, max_width = 0;
        int lines = 1;
        std::uint32_t prev = 0;
        float word_w = 0;                 // width of the pending word (since last break opportunity)
        const std::string_view str = run.str;
        std::size_t i = 0;
        while (i < str.size())
        {
            const std::uint32_t cp = utf8_next(str, i);
            if (cp == '\n')
            {
                max_width = std::max(max_width, width);
                width = 0; word_w = 0; prev = 0; ++lines;
                continue;
            }
            // Whole-pixel advances (rounded once per glyph, kerning folded in): consistent letter
            // spacing at minified SDF sizes. The shaper rounds identically — keep them in lockstep.
            const float adv =
                std::round((atlas->glyph(cp).xadvance + atlas->kerning(prev, cp)) * s);
            if (cp == ' ')
            {
                width += adv; word_w = 0; prev = cp;
                continue;
            }
            if (wrap > 0 && width + adv > wrap && width > 0 && word_w == 0)
            {
                // Wrap at the space boundary (word_w==0 means we're at a fresh word start).
                max_width = std::max(max_width, width);
                width = 0; ++lines;
            }
            width += adv; word_w += adv; prev = cp;
            if (wrap > 0 && width > wrap && word_w == width)
            {
                // A single word longer than the line: hard-break to avoid unbounded overflow.
                max_width = std::max(max_width, width - adv);
                width = adv; word_w = adv; ++lines;
            }
        }
        max_width = std::max(max_width, width);
        return { detail::to_u16(static_cast<int>(std::ceil(max_width))),
                 detail::to_u16(static_cast<int>(std::ceil(lines * atlas->line_height() * s))) };
    }
};

}  // namespace string
