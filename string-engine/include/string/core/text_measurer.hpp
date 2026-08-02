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

    // The bitmap-atlas measurer does not wrap: it exists for the fixed-size font path. Reporting a
    // zero floor and the unwrapped height keeps it a valid `measurer` and makes `.wrap()` a no-op
    // here rather than something that silently half-works.
    std::uint16_t min_width(const element&) const noexcept { return 0; }
    std::uint16_t height_at(const element& e, std::uint16_t) const noexcept
    {
        return (*this)(e).height;
    }
};

// The advance width of the word starting at byte offset `i` — up to the next space, newline or the
// end of the run — measured with `prev` as the preceding glyph so kerning at the join is included.
// Does not consume anything; the caller still walks the word glyph by glyph.
//
// THIS IS WHAT MAKES WORD WRAP WORD WRAP. A break must be decided from the width of the WHOLE
// upcoming word: asking only whether the next GLYPH fits is character wrap, and it looks correct
// right up until a word whose first letter fits and whose tail does not — which then overflows the
// box and is clipped mid-word. (Clay reaches the same place by pre-splitting every string into
// measured words; we scan ahead instead, which costs a second pass over each word and needs no
// cache. If that ever shows up in a profile, the cache is the known answer.)
[[nodiscard]] inline float measure_word(dynamic_font_atlas& atlas, std::string_view str,
                                        std::size_t i, float s, std::uint32_t prev)
{
    float w = 0;
    while (i < str.size())
    {
        const std::uint32_t cp = utf8_next(str, i);
        if (cp == ' ' || cp == '\n') break;
        w += std::round((atlas.glyph(cp).xadvance + atlas.kerning(prev, cp)) * s);
        prev = cp;
    }
    return w;
}

// A measurer over the dynamic (Unicode, grow-on-demand) atlas. Scales metrics by font_px / bake_px so
// a single reference-size SDF atlas renders text at any requested size, decodes UTF-8, applies
// kerning, honours explicit '\n', and word-wraps to `wrap_px` (0 = no wrap). Must match the shaper in
// ui_pass exactly so measured boxes fit the drawn glyphs. Non-const atlas: measuring caches glyphs.
struct dynamic_text_measurer
{
    dynamic_font_atlas* atlas;
    std::span<const text_run> texts;
    std::uint16_t wrap_px = 0;  // element cross-size wrap width; 0 = single unwrapped line(s)

    // The widest line and the line count for `run` at scale `s`, wrapping at `wrap` px (0 = never).
    //
    // Factored out because THREE callers need the same answer and must not disagree: the unwrapped
    // measure, the wrapped height, and — critically — the glyph shaper in ui_pass, which runs the
    // identical algorithm to place the glyphs. If any of them drifts, text draws a different number
    // of lines than the box reserves and overdraws whatever sits below it.
    struct shaped
    {
        float width;
        int lines;
    };

    shaped shape(const text_run& run, float s, float wrap) const
    {
        float width = 0, max_width = 0;
        int lines = 1;
        std::uint32_t prev = 0;
        float word_w = 0;                 // width of the pending word (since last break opportunity)
        // The pen position up to the last VISIBLE glyph on this line. A line broken after a space
        // carries that space in `width` — it is real layout distance, and the break decision needs
        // it — but the space draws nothing, so counting it in the line's width reports a line wider
        // than the box it was just wrapped into. (Clay trims the same thing; see finalCharIsSpace.)
        float visible = 0;
        const std::string_view str = run.str;
        std::size_t i = 0;
        while (i < str.size())
        {
            const std::size_t at = i;
            const std::uint32_t cp = utf8_next(str, i);
            if (cp == '\n')
            {
                // Explicit newlines and the end of the run keep the pre-wrap accounting (`width`,
                // trailing space and all) — that path predates wrapping and every existing screen is
                // measured through it. Only a WRAP break trims, below.
                max_width = std::max(max_width, width);
                width = 0; visible = 0; word_w = 0; prev = 0; ++lines;
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
            if (wrap > 0 && width > 0)
            {
                // Two break opportunities, and they ask different questions:
                //   * at a word START (word_w == 0), whether the WHOLE word still fits — the only
                //     test that is actually word wrap;
                //   * mid-word, only when the word IS the whole line (word_w == width), i.e. a
                //     single word too long to ever fit: hard-break it rather than overflow forever.
                const bool brk = word_w == 0
                    ? width + measure_word(*atlas, str, at, s, prev) > wrap
                    : (word_w == width && width + adv > wrap);
                if (brk)
                {
                    max_width = std::max(max_width, visible);   // not `width`: see `visible` above
                    width = 0; visible = 0; word_w = 0; ++lines;
                }
            }
            width += adv; word_w += adv; prev = cp;
            visible = width;   // this glyph is visible, so the line now extends to here
        }
        max_width = std::max(max_width, width);
        return { max_width, lines };
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
        const shaped sh = shape(*run, s, wrap);
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
        const shaped sh = shape(*run, s, static_cast<float>(width));
        return detail::to_u16(static_cast<int>(std::ceil(sh.lines * atlas->line_height() * s)));
    }
};

}  // namespace string
