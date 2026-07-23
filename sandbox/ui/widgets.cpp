#include "ui/widgets.hpp"

#include <algorithm>
#include <cmath>

#include "ui/sound.hpp"

namespace sandbox::ui
{
using namespace string;

namespace
{
color lerp_col(color a, color b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    auto mix = [t](uint8_t x, uint8_t y) {
        return static_cast<uint8_t>(std::lround(x + (y - x) * t));
    };
    return color{ mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a) };
}
}  // namespace

void begin_panel(layout_builder& b, color fill, color stroke, uint16_t radius, uint16_t pad,
                 uint16_t gap, direction dir)
{
    element panel{};
    panel.color = fill;
    panel.stroke_color = stroke;
    panel.stroke_width = stroke.a > 0 ? 2 : 0;
    panel.radius = radius;
    panel.shape = shape::ROUNDED_RECTANGLE;
    panel.sizing = size_fit();
    b.begin(panel, format{ .padding = { pad, pad, pad, pad }, .gap = gap, .direction = dir });
}

bool button(layout_builder& b, Motion& m, const Interaction& it, std::string_view id_name,
            std::string_view label, uint16_t font_px, uint16_t min_w)
{
    const id bid = make_id(id_name);
    const bool hot = it.is_hot(bid.hash);
    const bool focused = it.is_focused(bid.hash);
    const bool activated = it.is_pressed(bid.hash);

    // Motion: glide the fill toward the hover/idle target; press gives a brief brighter flash.
    const color idle = col::panel_alt;
    const color hover = lerp_col(col::panel_alt, col::accent, 0.55f);
    const color target = activated ? col::accent : (hot || focused ? hover : idle);
    const color fill = m.animate_color(bid.hash, target, { 0.14f, Curve::EASE_OUT });

    element btn{};
    btn.id = bid;
    btn.color = fill;
    btn.stroke_color = focused ? col::accent_warm : (hot ? col::stroke_hi : col::stroke);
    btn.stroke_width = focused ? 3 : 2;   // focus-visible ring (gamepad/keyboard nav)
    btn.radius = 8;
    btn.shape = shape::ROUNDED_RECTANGLE;
    const uint16_t w = std::max<uint16_t>(min_w, 0);
    btn.sizing = { min_w > 0 ? fit(w) : fit(), fit() };

    element text{};
    text.color = hot || focused ? col::text : col::text_dim;
    text.sizing = size_fit();

    b.begin(btn, format{ .padding = { 12, 12, 7, 7 }, .gap = 0, .alignment = alignment::CENTER,
                         .direction = direction::HORIZONTAL, .justify = justification::CENTER })
         .add_text(text, label, font_px)
     .end();
    if (activated)
        play_cue(Cue::Activate);
    return activated;
}

void progress_bar(layout_builder& b, uint16_t w, uint16_t h, float frac, color fill, color track)
{
    frac = std::clamp(frac, 0.0f, 1.0f);
    element t{};
    t.color = track;
    t.radius = h / 2;
    t.shape = shape::ROUNDED_RECTANGLE;
    t.sizing = size_fixed(w, h);

    element f{};
    f.color = fill;
    f.radius = h / 2;
    f.shape = shape::ROUNDED_RECTANGLE;
    f.sizing = size_fixed(static_cast<uint16_t>(std::max(1.0f, frac * w)), h);

    b.begin(t, format{ .direction = direction::HORIZONTAL })
         .add_element(f)
     .end();
}

void tooltip(layout_builder& b, uint16_t x, uint16_t y, std::string_view title, color title_col,
             const std::string* body_lines, std::size_t body_count, uint16_t wrap_px)
{
    element panel{};
    panel.floating = true;
    panel.float_x = x;
    panel.float_y = y;
    panel.color = col::panel;
    panel.stroke_color = col::stroke_hi;
    panel.stroke_width = 2;
    panel.radius = 10;
    panel.shape = shape::ROUNDED_RECTANGLE;
    panel.sizing = { fit(120), fit() };

    b.begin(panel, format{ .padding = { 10, 10, 8, 8 }, .gap = 4, .direction = direction::VERTICAL });
    element t{};
    t.color = title_col;
    t.sizing = size_fit();
    b.add_text(t, title, 22);
    for (std::size_t i = 0; i < body_count; ++i)
    {
        element row{};
        row.color = col::text_dim;
        row.sizing = { fixed(wrap_px), fit() };  // fixed width -> shaper word-wraps to it
        b.add_text(row, body_lines[i], 18);
    }
    b.end();
}

bool icon_cell(layout_builder& b, Motion& m, const Interaction& it, std::string_view id_name,
               std::string_view label, color border, uint16_t size, float cooldown, color fill)
{
    const id cid = make_id(id_name);
    const bool hot = it.is_hot(cid.hash);
    const bool focused = it.is_focused(cid.hash);
    const bool activated = it.is_pressed(cid.hash);

    // Hover/press lift: brighten the fill slightly.
    const color target = activated ? lerp_col(fill, col::accent, 0.5f)
                                   : (hot || focused ? lerp_col(fill, border, 0.35f) : fill);
    const color anim_fill = m.animate_color(cid.hash, target, { 0.12f, Curve::EASE_OUT });

    element cell{};
    cell.id = cid;
    cell.color = anim_fill;
    cell.stroke_color = focused ? col::accent_warm : border;
    cell.stroke_width = focused ? 3 : 2;
    cell.radius = 8;
    cell.shape = shape::ROUNDED_RECTANGLE;
    cell.sizing = size_fixed(size, size);
    cell.sweep = static_cast<uint8_t>(std::clamp(cooldown, 0.0f, 1.0f) * 255.0f);

    element text{};
    text.color = col::text;
    text.sizing = size_fit();

    b.begin(cell, format{ .padding = { 4, 4, 4, 4 }, .alignment = alignment::CENTER,
                          .direction = direction::VERTICAL, .justify = justification::CENTER })
         .add_text(text, label, 18)
     .end();
    if (activated)
        play_cue(Cue::Activate);
    return activated;
}

}  // namespace sandbox::ui
