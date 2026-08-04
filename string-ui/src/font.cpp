#include <string/ui/font.hpp>

#include <cstring>
#include <stdexcept>

// Single translation unit that compiles the vendored stb single-header impls. stb_rect_pack must
// precede stb_truetype (the latter references stbrp_* in its pack path).
#define STB_RECT_PACK_IMPLEMENTATION
#include <string/ui/stb_rect_pack.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include <string/ui/stb_truetype.h>

namespace string
{

namespace
{
// SDF bake parameters. `padding` is how many pixels of distance field surround each glyph;
// `onedge` is the field value that lands exactly on the glyph edge; `pixel_dist_scale` maps one
// pixel of distance to `onedge/padding` field units so the field spans [0, 2*onedge] across the
// padded band. These are baked into font_atlas so the shader can threshold consistently.
constexpr int kPadding = 4;
constexpr unsigned char kOnedge = 128;
constexpr float kPixelDistScale = static_cast<float>(kOnedge) / kPadding;
}  // namespace

const glyph_metrics& font_atlas::glyph(char c) const
{
    if (c < kFirstGlyph || c > kLastGlyph)
    {
        c = '?';
    }
    return glyphs[static_cast<std::size_t>(c - kFirstGlyph)];
}

font_atlas build_font_atlas(std::span<const std::uint8_t> ttf, float pixel_height,
                            std::uint32_t atlas_w, std::uint32_t atlas_h)
{
    stbtt_fontinfo info;
    const int offset = stbtt_GetFontOffsetForIndex(ttf.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&info, ttf.data(), offset))
    {
        throw std::runtime_error("font: stbtt_InitFont failed (not a valid TrueType blob)");
    }

    const float scale = stbtt_ScaleForPixelHeight(&info, pixel_height);

    font_atlas atlas;
    atlas.pixel_height = pixel_height;
    atlas.sdf_onedge = static_cast<float>(kOnedge) / 255.0f;
    atlas.sdf_pixel_dist = kPixelDistScale;

    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    atlas.ascent = asc * scale;
    atlas.descent = desc * scale;
    atlas.line_gap = gap * scale;

    // Rasterise each printable glyph to an owned SDF bitmap and record its advance. Space (and any
    // empty glyph) has no bitmap but still advances.
    struct RasterGlyph
    {
        unsigned char* bitmap = nullptr;
        int w = 0, h = 0, xoff = 0, yoff = 0;
    };
    std::array<RasterGlyph, kGlyphCount> raster{};

    for (std::size_t i = 0; i < kGlyphCount; ++i)
    {
        const int codepoint = kFirstGlyph + static_cast<int>(i);
        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&info, codepoint, &advance, &lsb);
        atlas.glyphs[i].xadvance = advance * scale;

        RasterGlyph& g = raster[i];
        g.bitmap = stbtt_GetCodepointSDF(&info, scale, codepoint, kPadding, kOnedge, kPixelDistScale,
                                         &g.w, &g.h, &g.xoff, &g.yoff);
    }

    // Rectangle-pack the non-empty glyphs into the atlas. Retry once at double height on overflow.
    auto pack_into = [&](std::uint32_t w, std::uint32_t h) -> bool {
        std::vector<stbrp_rect> rects;
        rects.reserve(kGlyphCount);
        for (std::size_t i = 0; i < kGlyphCount; ++i)
        {
            if (raster[i].bitmap == nullptr || raster[i].w <= 0 || raster[i].h <= 0)
            {
                continue;
            }
            stbrp_rect r{};
            r.id = static_cast<int>(i);
            r.w = static_cast<stbrp_coord>(raster[i].w);
            r.h = static_cast<stbrp_coord>(raster[i].h);
            rects.push_back(r);
        }

        stbrp_context ctx;
        std::vector<stbrp_node> nodes(w);
        stbrp_init_target(&ctx, static_cast<int>(w), static_cast<int>(h), nodes.data(),
                          static_cast<int>(nodes.size()));
        if (stbrp_pack_rects(&ctx, rects.data(), static_cast<int>(rects.size())) == 0)
        {
            return false;  // at least one rect didn't fit
        }

        atlas.atlas_w = w;
        atlas.atlas_h = h;
        atlas.pixels.assign(static_cast<std::size_t>(w) * h, 0);
        for (const stbrp_rect& r : rects)
        {
            const std::size_t i = static_cast<std::size_t>(r.id);
            const RasterGlyph& g = raster[i];
            // Blit the glyph's SDF into the atlas at its packed slot.
            for (int row = 0; row < g.h; ++row)
            {
                std::memcpy(&atlas.pixels[(static_cast<std::size_t>(r.y) + row) * w + r.x],
                            &g.bitmap[static_cast<std::size_t>(row) * g.w],
                            static_cast<std::size_t>(g.w));
            }
            glyph_metrics& m = atlas.glyphs[i];
            m.u0 = static_cast<float>(r.x) / w;
            m.v0 = static_cast<float>(r.y) / h;
            m.u1 = static_cast<float>(r.x + g.w) / w;
            m.v1 = static_cast<float>(r.y + g.h) / h;
            m.xoff = static_cast<float>(g.xoff);
            m.yoff = static_cast<float>(g.yoff);
            m.w = static_cast<std::uint16_t>(g.w);
            m.h = static_cast<std::uint16_t>(g.h);
        }
        return true;
    };

    bool packed = pack_into(atlas_w, atlas_h);
    if (!packed)
    {
        packed = pack_into(atlas_w, atlas_h * 2);
    }

    for (RasterGlyph& g : raster)
    {
        if (g.bitmap != nullptr)
        {
            stbtt_FreeSDF(g.bitmap, nullptr);
        }
    }

    if (!packed)
    {
        throw std::runtime_error("font: glyph atlas packing overflowed (font too large for atlas)");
    }
    return atlas;
}

}  // namespace string
