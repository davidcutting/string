#include <cstring>

#include <string/ui/layout.hpp>
#include <string/ui/text_shaper.hpp>

namespace string
{
namespace
{

// Key over the three things a shaped run depends on. Scale and wrap go in by their BIT PATTERN
// rather than quantized: identical inputs produce identical bits, so there is no tolerance to pick
// and no pair of nearly-equal scales that silently share an entry. The stored inputs are verified on
// every hit anyway, so this only has to spread well.
std::uint64_t shape_key(std::string_view s, float scale, float wrap) noexcept
{
    std::uint64_t h = fnv1a(s);
    const auto mix = [&h](float f) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        h ^= bits;
        h *= 0x100000001b3ull;
    };
    mix(scale);
    mix(wrap);
    return h;
}

}  // namespace

const text_shape_cache::shaped_run& text_shape_cache::get(dynamic_font_atlas& atlas,
                                                          std::string_view str, float scale,
                                                          float wrap)
{
    const std::uint64_t key = shape_key(str, scale, wrap);
    entry& e = entries_[key];

    // VERIFIED, not assumed. A hash collision here would draw the wrong text — louder than a stale
    // layout — and these strings are a handful of bytes, so comparing them costs far less than the
    // shaping it avoids. On a mismatch the entry is simply recomputed, so a collision degrades to a
    // miss rather than to a defect.
    const bool valid = !e.text.empty() && e.text == str && e.scale == scale && e.wrap == wrap;
    if (valid)
    {
        ++hits_;
        e.seen = true;
        return e.run;
    }

    ++misses_;
    e.text.assign(str);
    e.scale = scale;
    e.wrap = wrap;
    e.seen = true;
    e.run.glyphs.clear();
    e.run.metrics = shape_text(atlas, str, scale, wrap, [&e](const placed_glyph& g) {
        e.run.glyphs.push_back(g);
    });
    return e.run;
}

void text_shape_cache::end_frame()
{
    // Same rule as Motion: anything not touched this frame is dropped. A scrolled-away table stops
    // pinning its rows, and a cache over a UI that changes every frame cannot grow without bound.
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (!it->second.seen)
            it = entries_.erase(it);
        else
        {
            it->second.seen = false;
            ++it;
        }
    }
}

}  // namespace string
