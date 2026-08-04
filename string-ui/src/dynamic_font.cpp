#include <string/ui/dynamic_font.hpp>

#include <cstring>
#include <stdexcept>

// stb_truetype's implementation is compiled in font.cpp (with stb_rect_pack). Here we only need the
// declarations, so include WITHOUT the IMPLEMENTATION macro.
#include <string/ui/stb_truetype.h>

namespace string
{

namespace
{
// SDF bake parameters. A wider padding than the baked-ASCII path (kPadding 4) gives a bigger distance
// spread so downscaled small text stays crisp with the shader's fwidth AA.
constexpr int kPadding = 6;
constexpr unsigned char kOnedge = 128;
constexpr float kPixelDistScale = static_cast<float>(kOnedge) / kPadding;
constexpr std::uint32_t kShelfPad = 1;  // 1px gutter between glyphs so bilinear taps don't bleed
}  // namespace

struct dynamic_font_atlas::Impl
{
    stbtt_fontinfo info{};
};

std::uint32_t utf8_next(std::string_view s, std::size_t& i)
{
    if (i >= s.size())
        return 0xFFFD;
    const auto b0 = static_cast<unsigned char>(s[i]);
    auto cont = [&](std::size_t k) -> int {
        if (i + k >= s.size())
            return -1;
        const auto b = static_cast<unsigned char>(s[i + k]);
        return (b & 0xC0) == 0x80 ? (b & 0x3F) : -1;
    };
    if (b0 < 0x80)
    {
        ++i;
        return b0;
    }
    if ((b0 & 0xE0) == 0xC0)
    {
        const int c1 = cont(1);
        if (c1 < 0) { ++i; return 0xFFFD; }
        i += 2;
        return (static_cast<std::uint32_t>(b0 & 0x1F) << 6) | c1;
    }
    if ((b0 & 0xF0) == 0xE0)
    {
        const int c1 = cont(1), c2 = cont(2);
        if (c1 < 0 || c2 < 0) { ++i; return 0xFFFD; }
        i += 3;
        return (static_cast<std::uint32_t>(b0 & 0x0F) << 12) | (static_cast<std::uint32_t>(c1) << 6) | c2;
    }
    if ((b0 & 0xF8) == 0xF0)
    {
        const int c1 = cont(1), c2 = cont(2), c3 = cont(3);
        if (c1 < 0 || c2 < 0 || c3 < 0) { ++i; return 0xFFFD; }
        i += 4;
        return (static_cast<std::uint32_t>(b0 & 0x07) << 18) | (static_cast<std::uint32_t>(c1) << 12)
             | (static_cast<std::uint32_t>(c2) << 6) | c3;
    }
    ++i;  // invalid lead byte
    return 0xFFFD;
}

dynamic_font_atlas::dynamic_font_atlas(std::span<const std::uint8_t> ttf, float bake_px,
                                       std::uint32_t atlas_w, std::uint32_t atlas_h)
: impl_(std::make_unique<Impl>())
, ttf_(ttf.begin(), ttf.end())
, bake_px_(bake_px)
, atlas_w_(atlas_w)
, atlas_h_(atlas_h)
{
    const int offset = stbtt_GetFontOffsetForIndex(ttf_.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&impl_->info, ttf_.data(), offset))
        throw std::runtime_error("dynamic_font: stbtt_InitFont failed (not a valid TrueType blob)");

    scale_ = stbtt_ScaleForPixelHeight(&impl_->info, bake_px_);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&impl_->info, &asc, &desc, &gap);
    ascent_ = asc * scale_;
    descent_ = desc * scale_;
    line_gap_ = gap * scale_;
    sdf_onedge_ = static_cast<float>(kOnedge) / 255.0f;

    reset_atlas();
    glyph('?');  // seed the fallback so a miss never fails to find a metric
}

dynamic_font_atlas::~dynamic_font_atlas() = default;

void dynamic_font_atlas::reset_atlas()
{
    pixels_.assign(static_cast<std::size_t>(atlas_w_) * atlas_h_, 0);
    shelf_x_ = kShelfPad;
    shelf_y_ = kShelfPad;
    shelf_h_ = 0;
    cache_.clear();
    ++generation_;
    dirty_ = true;
}

bool dynamic_font_atlas::pack(int w, int h, std::uint32_t& out_x, std::uint32_t& out_y)
{
    const auto uw = static_cast<std::uint32_t>(w);
    const auto uh = static_cast<std::uint32_t>(h);
    if (uw + 2 * kShelfPad > atlas_w_ || uh + 2 * kShelfPad > atlas_h_)
        return false;  // single glyph larger than the whole atlas
    if (shelf_x_ + uw + kShelfPad > atlas_w_)
    {
        // New shelf.
        shelf_y_ += shelf_h_ + kShelfPad;
        shelf_x_ = kShelfPad;
        shelf_h_ = 0;
    }
    if (shelf_y_ + uh + kShelfPad > atlas_h_)
        return false;  // atlas full
    out_x = shelf_x_;
    out_y = shelf_y_;
    shelf_x_ += uw + kShelfPad;
    shelf_h_ = std::max(shelf_h_, uh);
    return true;
}

const glyph_metrics& dynamic_font_atlas::glyph(std::uint32_t codepoint)
{
    if (auto it = cache_.find(codepoint); it != cache_.end())
        return it->second;

    // Map an unrepresentable codepoint to '?' (already cached after construction).
    if (stbtt_FindGlyphIndex(&impl_->info, static_cast<int>(codepoint)) == 0 && codepoint != '?')
    {
        const glyph_metrics& fallback = glyph('?');
        return cache_.emplace(codepoint, fallback).first->second;
    }

    int advance = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&impl_->info, static_cast<int>(codepoint), &advance, &lsb);

    glyph_metrics m{};
    m.xadvance = advance * scale_;

    int gw = 0, gh = 0, xoff = 0, yoff = 0;
    unsigned char* sdf = stbtt_GetCodepointSDF(&impl_->info, scale_, static_cast<int>(codepoint),
                                               kPadding, kOnedge, kPixelDistScale, &gw, &gh, &xoff, &yoff);
    if (sdf != nullptr && gw > 0 && gh > 0)
    {
        std::uint32_t x = 0, y = 0;
        if (!pack(gw, gh, x, y))
        {
            reset_atlas();
            // reset cleared the cache incl. '?'; re-seed it, then retry this glyph once.
            stbtt_FreeSDF(sdf, nullptr);
            glyph('?');
            if (auto it = cache_.find(codepoint); it != cache_.end())
                return it->second;
            sdf = stbtt_GetCodepointSDF(&impl_->info, scale_, static_cast<int>(codepoint),
                                        kPadding, kOnedge, kPixelDistScale, &gw, &gh, &xoff, &yoff);
            if (sdf == nullptr || !pack(gw, gh, x, y))
            {
                if (sdf) stbtt_FreeSDF(sdf, nullptr);
                return cache_.emplace(codepoint, m).first->second;  // no bitmap: advance only
            }
        }
        for (int row = 0; row < gh; ++row)
            std::memcpy(&pixels_[(static_cast<std::size_t>(y) + row) * atlas_w_ + x],
                        &sdf[static_cast<std::size_t>(row) * gw], static_cast<std::size_t>(gw));
        m.u0 = static_cast<float>(x) / atlas_w_;
        m.v0 = static_cast<float>(y) / atlas_h_;
        m.u1 = static_cast<float>(x + gw) / atlas_w_;
        m.v1 = static_cast<float>(y + gh) / atlas_h_;
        m.xoff = static_cast<float>(xoff);
        m.yoff = static_cast<float>(yoff);
        m.w = static_cast<std::uint16_t>(gw);
        m.h = static_cast<std::uint16_t>(gh);
        dirty_ = true;
    }
    if (sdf != nullptr)
        stbtt_FreeSDF(sdf, nullptr);

    return cache_.emplace(codepoint, m).first->second;
}

float dynamic_font_atlas::kerning(std::uint32_t prev, std::uint32_t codepoint) const
{
    if (prev == 0)
        return 0.0f;
    const int k = stbtt_GetCodepointKernAdvance(&impl_->info, static_cast<int>(prev),
                                                static_cast<int>(codepoint));
    return k * scale_;
}

}  // namespace string
