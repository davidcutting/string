#pragma once

#include <cmath>
#include <cstdint>
#include <string_view>

#include <string/ui/dynamic_font.hpp>

#include <unordered_map>
#include <vector>

// THE text shaper. One walk over a run that both MEASURES it and PLACES its glyphs.
//
// It exists because those two jobs used to be two implementations of the same algorithm, in two
// different libraries: `dynamic_text_measurer::shape` in the UI kit decided how wide and how tall a
// text element is, and `append_glyphs` in the renderer's UI pass decided where each glyph goes. Both
// walked UTF-8, applied kerning, rounded advances to whole pixels and made the same two word-wrap
// break decisions — and each carried a comment warning that it had to match the other exactly or
// text would draw a different number of lines than its box reserved and overdraw whatever sat below.
//
// That is a correctness hazard maintained by discipline across a library boundary, which is the kind
// that survives review and fails in the field. So: one function, one set of break rules, and the two
// callers differ only in what they do with the result.
//
// LOCAL COORDINATES, deliberately. Glyphs come back positioned relative to the run's own top-left,
// so the shaper never learns where on screen a node landed — the caller adds its box origin.
//
// WHICH IS WHY GLYPH POSITIONS COME BACK UNROUNDED, and the caller snaps them after translating.
// Rounding here instead looks equivalent and is not: `std::round` rounds halves AWAY FROM ZERO, so it
// does not commute with adding an integer across the sign boundary — round(5 + -0.5) == 5 while
// 5 + round(-0.5) == 4. A glyph's left side bearing is frequently negative, and the FIRST glyph of a
// run is the one whose local pen is 0, so that is exactly where it bites. Snapping locally moved the
// leading character of a great many runs by one pixel, which a pixel gate caught and nothing else
// would have.
//
// ADVANCES are still rounded here, and must be: whole-pixel advances are what keep letter spacing
// even at minified SDF sizes, they are a property of the run rather than of where it is drawn, and
// measurement depends on them. Positions are a rasterisation concern; advances are a shaping one.
namespace string
{

// Where one glyph landed, in run-local pixels: (x, y) is the quad's top-left, UNSNAPPED — a drawing
// caller adds its box origin and rounds, in that order (see the header note on why).
//
// NOTE that (w, h) is the ATLAS BITMAP's size, not the glyph's ink extent — for SDF glyphs it
// includes spread padding on every side. So a glyph legitimately draws a pixel or two outside the
// box the metrics reserved for it, exactly as overhangs and italic tails do in any text stack.
// Do not treat these as bounds to assert against a measured box.
struct placed_glyph
{
    const glyph_metrics* metrics;   // for the atlas UVs; never null
    float x;
    float y;
    float w;
    float h;
    // 0-based line this glyph landed on. The one number that lets a caller check its own placement
    // against the `lines` the same call reports — which is precisely the agreement that used to be
    // maintained by hand between the measurer and the renderer's shaper.
    int line;
};

struct shaped_text
{
    float width;   // the widest line, with any trailing space on a WRAPPED line trimmed off
    int lines;
};

// A sink that measures only. `shape_text` is used for measurement far more often than for drawing,
// and an empty lambda is the same thing spelled at every call site.
struct no_glyphs
{
    constexpr void operator()(const placed_glyph&) const noexcept {}
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

// Walk `str` at `scale`, wrapping at `wrap` px (0 = never), invoking `sink` once per DRAWABLE glyph.
//
// Zero-area glyphs (space, and anything the font renders empty) advance the pen but are not emitted,
// which is why a sink can assume everything it receives has a quad worth drawing.
template <typename Sink>
shaped_text shape_text(dynamic_font_atlas& atlas, std::string_view str, float scale, float wrap,
                       Sink&& sink)
{
    const float line_h = atlas.line_height() * scale;

    float width = 0;        // pen advance on the current line, from its left edge
    float max_width = 0;
    int lines = 1;
    std::uint32_t prev = 0;
    float word_w = 0;       // width of the pending word (since the last break opportunity)
    // The pen position up to the last VISIBLE glyph on this line. A line broken after a space
    // carries that space in `width` — it is real layout distance, and the break decision needs it —
    // but the space draws nothing, so counting it would report a line wider than the box it was
    // just wrapped into. (Clay trims the same thing; see finalCharIsSpace.)
    float visible = 0;
    float baseline = atlas.ascent() * scale;

    std::size_t i = 0;
    while (i < str.size())
    {
        const std::size_t at = i;
        const std::uint32_t cp = utf8_next(str, i);
        if (cp == '\n')
        {
            // Explicit newlines keep the pre-wrap accounting (`width`, trailing space and all): that
            // path predates wrapping and every existing screen is measured through it. Only a WRAP
            // break trims, below.
            max_width = std::max(max_width, width);
            width = 0; visible = 0; word_w = 0; prev = 0; ++lines;
            baseline += line_h;
            continue;
        }

        const glyph_metrics& g = atlas.glyph(cp);
        // Whole-pixel advances, rounded ONCE per glyph with kerning folded in: consistent letter
        // spacing at minified SDF sizes, and the reason measurement and placement can agree exactly.
        const float adv = std::round((g.xadvance + atlas.kerning(prev, cp)) * scale);

        if (cp == ' ')
        {
            width += adv; word_w = 0; prev = cp;
            continue;
        }

        if (wrap > 0 && width > 0)
        {
            // Two break opportunities, asking different questions:
            //   * at a word START (word_w == 0), whether the WHOLE word still fits — the only test
            //     that is actually word wrap. Asking about this glyph alone is character wrap, and
            //     leaves the tail of a word hanging out of the box.
            //   * mid-word, only when the word IS the whole line (word_w == width): a single word
            //     too long to ever fit, hard-broken rather than overflowing forever.
            const bool brk = word_w == 0
                ? width + measure_word(atlas, str, at, scale, prev) > wrap
                : (word_w == width && width + adv > wrap);
            if (brk)
            {
                max_width = std::max(max_width, visible);   // not `width`: see `visible` above
                width = 0; visible = 0; word_w = 0; ++lines;
                baseline += line_h;
                // `prev` is deliberately NOT reset here (an explicit '\n' does reset it). Kerning
                // therefore carries across a wrap break. Both previous implementations did this;
                // it is preserved rather than quietly fixed, because changing it would re-flow
                // measured text everywhere at once.
            }
        }

        if (g.w > 0 && g.h > 0)
        {
            // NOT rounded — see the header. `width` and `baseline` are already whole-pixel (advances
            // are snapped as they accumulate); it is the BEARING that carries the fraction, and it
            // has to be added to a screen position before it is snapped.
            sink(placed_glyph{ &g,
                               width + g.xoff * scale,
                               baseline + g.yoff * scale,
                               g.w * scale,
                               g.h * scale,
                               lines - 1 });
        }

        width += adv; word_w += adv; prev = cp;
        visible = width;   // this glyph is visible, so the line now extends to here
    }

    max_width = std::max(max_width, width);
    return { max_width, lines };
}


// --- The shaping cache (brief 12b, M0) ---------------------------------------------------------
//
// Shaping is ~95% of measured UI layout cost, and every string is shaped TWICE per frame: once by
// `dynamic_text_measurer` to size it, once by the renderer's UI pass to place its quads. Both now
// call the SAME `shape_text` (which fixed a drift hazard), and this collects the other half of that
// unification: one cached ANSWER instead of two identical walks.
//
// The same idea as `dynamic_font_atlas` one level up — that caches rasterized glyphs, this caches
// the run they compose into.
//
// Atlas growth deliberately does NOT invalidate: glyphs bake once at `bake_px` and their metrics are
// stable, so a new codepoint only ADDS entries. A full atlas re-bake would need an epoch in the key;
// that is not a thing today, so it is noted rather than built.
class text_shape_cache
{
public:
    struct shaped_run
    {
        shaped_text metrics;
        // UNCLIPPED and run-local. The renderer clips glyphs to the node box, and clipping BEFORE
        // caching would poison the entry: the same string replayed at a different scroll offset
        // would come back missing the glyphs that happened to be outside the box the first time.
        // Clipping is therefore a replay-time concern, and this holds the whole run.
        std::vector<placed_glyph> glyphs;
    };

    // The shaped run for these inputs, computed on first sight and reused afterwards.
    [[nodiscard]] const shaped_run& get(dynamic_font_atlas& atlas, std::string_view str, float scale,
                                        float wrap);

    // Frame-aging eviction, same rule as `Motion::end_frame`: anything not touched this frame is
    // dropped, so a scrolled-away table does not pin its rows forever. Call once per frame.
    void end_frame();

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::uint64_t hits() const noexcept { return hits_; }
    [[nodiscard]] std::uint64_t misses() const noexcept { return misses_; }
    void reset_stats() noexcept { hits_ = misses_ = 0; }

private:
    struct entry
    {
        // The inputs are STORED, not just hashed, and verified on every hit.
        //
        // A hash collision here would render the WRONG TEXT, which is louder than the stale layout a
        // collision in brief 12b's M3 signature would cause — and these strings average a handful of
        // bytes, so keeping them costs almost nothing next to the glyph vector. Verify, don't gamble.
        std::string text;
        float scale = 0.0f;
        float wrap = 0.0f;
        shaped_run run;
        bool seen = true;
    };

    std::unordered_map<std::uint64_t, entry> entries_;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
};
}  // namespace string
