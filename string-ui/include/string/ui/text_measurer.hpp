#pragma once

#include <algorithm>
#include <cmath>
#include <span>

#include <string/ui/dynamic_font.hpp>
#include <string/ui/font.hpp>
#include <string/ui/layout.hpp>
#include <string/ui/text_shaper.hpp>

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

    // The bitmap-atlas measurer does not wrap: it exists for the fixed-size font path. Reporting a
    // zero floor and the unwrapped height keeps it a valid `measurer` and makes `.wrap()` a no-op
    // here rather than something that silently half-works.
    std::uint16_t min_width(const element&) const noexcept { return 0; }
    std::uint16_t height_at(const element& e, std::uint16_t) const noexcept
    {
        return (*this)(e).height;
    }
};

// A measurer over the dynamic (Unicode, grow-on-demand) atlas. Scales metrics by font_px / bake_px so
// a single reference-size SDF atlas renders text at any requested size, decodes UTF-8, applies
// kerning, honours explicit '\n', and word-wraps to `wrap_px` (0 = no wrap). Non-const atlas:
// measuring caches glyphs.
//
// It does none of that itself: every question here is answered by `shape_text` (text_shaper.hpp),
// the same call the renderer's UI pass makes to PLACE the glyphs. That is the point — measurement
// and drawing are one algorithm asked two questions, not two algorithms kept in step by hand.
struct dynamic_text_measurer
{
    dynamic_font_atlas* atlas;
    std::span<const text_run> texts;
    std::uint16_t wrap_px = 0;  // element cross-size wrap width; 0 = single unwrapped line(s)
    // Optional shaping cache (brief 12b M0). Null shapes directly, which is what the headless unit
    // tests do — the cache is an optimization and never a correctness dependency.
    text_shape_cache* cache = nullptr;

    // The widest line and the line count for `run` at scale `s`, wrapping at `wrap` px (0 = never).
    shaped_text shape(const text_run& run, float s, float wrap) const
    {
        // A HIT HERE ALSO SERVES THE RENDERER: the same entry carries the glyph placements the UI
        // pass replays, so the two shapes-per-string-per-frame this used to cost become one
        // computation and two lookups.
        if (cache != nullptr) return cache->get(*atlas, run.str, s, wrap).metrics;
        return shape_text(*atlas, run.str, s, wrap, no_glyphs{});
    }

    [[nodiscard]] const text_run* run_of(const element& e) const noexcept
    {
        if (e.text == 0 || atlas == nullptr || e.text >= texts.size()) return nullptr;
        return &texts[e.text];
    }

    [[nodiscard]] float scale_of(const text_run& run) const noexcept
    {
        const float px = run.font_px > 0 ? static_cast<float>(run.font_px) : atlas->bake_px();
        return px / atlas->bake_px();
    }

    dimension operator()(const element& e) const
    {
        const text_run* run = run_of(e);
        if (run == nullptr) return { 0, 0 };
        const float s = scale_of(*run);

        // Wrap width comes from the ELEMENT: a fixed-width text element word-wraps to exactly that
        // width (its box), everything else is unwrapped (single line, clipped by the box).
        //
        // An element that opted into `.wrap()` measures UNWRAPPED here even if it is fixed-width,
        // because THIS call is asking for its preferred width. Its height is corrected by
        // `height_at` once the width pass has decided how wide it actually is.
        const float wrap = e.wrap ? 0.0f
                                  : static_cast<float>(e.sizing.width.mode == size_mode::FIXED
                                                           ? e.sizing.width.value : wrap_px);
        const shaped_text sh = shape(*run, s, wrap);
        return { detail::to_u16(static_cast<int>(std::ceil(sh.width))),
                 detail::to_u16(static_cast<int>(std::ceil(sh.lines * atlas->line_height() * s))) };
    }

    // The widest single WORD — the narrowest the element can be without breaking a word in half.
    // Same advance rounding as `shape`, or the floor would disagree with the wrapping it exists to
    // protect.
    std::uint16_t min_width(const element& e) const
    {
        const text_run* run = run_of(e);
        if (run == nullptr) return 0;
        const float s = scale_of(*run);

        float word = 0, widest = 0;
        std::uint32_t prev = 0;
        std::size_t i = 0;
        const std::string_view str = run->str;
        while (i < str.size())
        {
            const std::uint32_t cp = utf8_next(str, i);
            if (cp == ' ' || cp == '\n')
            {
                widest = std::max(widest, word);
                word = 0;
                prev = 0;
                continue;
            }
            word += std::round((atlas->glyph(cp).xadvance + atlas->kerning(prev, cp)) * s);
            prev = cp;
        }
        widest = std::max(widest, word);
        return detail::to_u16(static_cast<int>(std::ceil(widest)));
    }

    // The height the run needs once wrapped to `width`. The one call that can only be answered after
    // the layout's width pass has run.
    std::uint16_t height_at(const element& e, std::uint16_t width) const
    {
        const text_run* run = run_of(e);
        if (run == nullptr) return 0;
        const float s = scale_of(*run);
        const shaped_text sh = shape(*run, s, static_cast<float>(width));
        return detail::to_u16(static_cast<int>(std::ceil(sh.lines * atlas->line_height() * s)));
    }
};

}  // namespace string
