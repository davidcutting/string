#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <string/ui/font.hpp>

namespace string
{

// A dynamic, grow-on-demand SDF glyph atlas (brief 05, milestone 2). Unlike font_atlas — which bakes
// a fixed ASCII range up front — this keeps the parsed font resident and rasterises each glyph the
// first time it is requested BY UNICODE CODEPOINT, packing it into a shelf-allocated R8 atlas. This
// is what player-name-safe Unicode needs: CJK/Cyrillic/accented names appear the moment they're used,
// with no fixed codepoint table.
//
// SDF is scale-independent, so glyphs are baked ONCE at a reference pixel size (bake_px) and the
// consumer scales metrics by font_px / bake_px at layout/draw time. Baking larger than the on-screen
// size (bake_px default 48) plus a wider SDF spread fixes the "not super crisp" small-text issue.
//
// The atlas texture lives on the CPU (pixels); the renderer uploads it and re-uploads the dirty
// region after any frame that added glyphs (see take_dirty). On overflow the atlas resets (clears +
// forgets cached glyphs); at 1024^2 with ~48px glyphs that holds hundreds of distinct glyphs, ample
// for names/UI. Not thread-safe — call from the render thread that owns the UI.
class dynamic_font_atlas
{
public:
    // Parse `ttf` and prepare an empty `atlas_w` x `atlas_h` R8 atlas. `bake_px` is the SDF reference
    // size. Throws on an invalid font.
    dynamic_font_atlas(std::span<const std::uint8_t> ttf, float bake_px = 48.0f,
                       std::uint32_t atlas_w = 1024, std::uint32_t atlas_h = 1024);
    ~dynamic_font_atlas();

    dynamic_font_atlas(const dynamic_font_atlas&) = delete;
    dynamic_font_atlas& operator=(const dynamic_font_atlas&) = delete;

    // Metrics for `codepoint`, rasterising + packing it on first sight (falls back to U+FFFD/'?' for
    // an unrepresentable codepoint). Values are in bake_px units — scale by font_px / bake_px().
    const glyph_metrics& glyph(std::uint32_t codepoint);

    // Kerning advance (bake_px units) between two codepoints (0 if the font has no kern pairs).
    float kerning(std::uint32_t prev, std::uint32_t codepoint) const;

    float bake_px() const { return bake_px_; }
    float ascent() const { return ascent_; }
    float descent() const { return descent_; }
    float line_gap() const { return line_gap_; }
    float line_height() const { return ascent_ - descent_ + line_gap_; }

    std::uint32_t atlas_w() const { return atlas_w_; }
    std::uint32_t atlas_h() const { return atlas_h_; }
    const std::uint8_t* pixels() const { return pixels_.data(); }
    std::size_t pixels_size() const { return pixels_.size(); }

    // SDF decode params (mirror font_atlas): edge value + generation counter (bumps on a reset so the
    // renderer can re-upload the WHOLE atlas rather than a stale dirty region).
    float sdf_onedge() const { return sdf_onedge_; }
    std::uint64_t generation() const { return generation_; }

    // Whether any glyph was added (or the atlas reset) since the last take_dirty(); clears the flag.
    bool take_dirty() { const bool d = dirty_; dirty_ = false; return d; }

private:
    struct Impl;                       // hides stbtt_fontinfo (keeps the header stb-free)
    std::unique_ptr<Impl> impl_;

    void reset_atlas();
    // Pack a w x h glyph onto a shelf; returns false if it doesn't fit (caller resets + retries).
    bool pack(int w, int h, std::uint32_t& out_x, std::uint32_t& out_y);

    std::vector<std::uint8_t> ttf_;    // owned copy (stbtt_fontinfo points into it)
    float bake_px_ = 48.0f;
    float scale_ = 0.0f;
    float ascent_ = 0, descent_ = 0, line_gap_ = 0;
    float sdf_onedge_ = 0;

    std::uint32_t atlas_w_ = 0, atlas_h_ = 0;
    std::vector<std::uint8_t> pixels_;
    // Shelf packer state.
    std::uint32_t shelf_x_ = 0;        // pen x on the current shelf
    std::uint32_t shelf_y_ = 0;        // top of the current shelf
    std::uint32_t shelf_h_ = 0;        // height of the current shelf

    std::unordered_map<std::uint32_t, glyph_metrics> cache_;
    bool dirty_ = false;
    std::uint64_t generation_ = 0;
};

// UTF-8 decoder: reads one codepoint from `s` starting at byte `i`, advancing `i` past it. Returns
// U+FFFD on a malformed sequence (and advances one byte so callers can't loop forever). Free function
// so the shaper/measurer share one decode path.
std::uint32_t utf8_next(std::string_view s, std::size_t& i);

}  // namespace string
