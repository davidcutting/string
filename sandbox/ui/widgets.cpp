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

void begin_panel(Ui& u, color fill, color stroke, uint16_t radius, uint16_t pad, uint16_t gap,
                 direction dir)
{
    // Still an explicit begin (the caller closes with u.builder().end()) because a panel here wraps
    // an arbitrary caller-authored body; the closure form is u.panel(name).content(...).
    element panel{};
    panel.color = fill;
    panel.stroke_color = stroke;
    panel.stroke_width = stroke.a > 0 ? 2 : 0;
    panel.radius = radius;
    panel.shape = shape::ROUNDED_RECTANGLE;
    panel.sizing = size_fit();
    u.builder().begin(panel,
                      format{ .padding = { pad, pad, pad, pad }, .gap = gap, .direction = dir });
}

bool button(Ui& u, std::string_view id_name, std::string_view label, uint16_t font_px,
            uint16_t min_w)
{
    const id bid = make_id(id_name);
    const bool hot = u.interaction_state().is_hot(bid.hash);
    const bool focused = u.interaction_state().is_focused(bid.hash);
    const bool activated = u.interaction_state().is_pressed(bid.hash);

    // Motion: glide the fill toward the hover/idle target; press gives a brief brighter flash.
    const color idle = theme().panel_alt;
    const color hover = lerp_col(theme().panel_alt, theme().accent, 0.55f);
    const color target = activated ? theme().accent : (hot || focused ? hover : idle);
    const color fill = u.motion().animate_color(bid.hash, target, { 0.14f, Curve::EASE_OUT });

    u.element(id_name)
        .color(fill)
        .stroke(focused ? theme().accent_warm : (hot ? theme().stroke_hi : theme().stroke),
                focused ? 3 : 2)   // focus-visible ring (gamepad/keyboard nav)
        .radius(8)
        .shape(shape::ROUNDED_RECTANGLE)
        .size({ min_w > 0 ? fit(min_w) : fit(), fit() })
        .pad({ 12, 12, 7, 7 })
        .align(alignment::CENTER)
        .row()
        .justify(justification::CENTER)
        .content([&](Ui& u2) {
            // The caption is DELIBERATELY id-less: hit_test resolves through it to the button, so
            // naming it would steal the button's own hover/click target.
            u2.text(label).color(hot || focused ? theme().text : theme().text_dim).font(font_px);
        });

    if (activated)
        play_cue(Cue::Activate);
    return activated;
}

void progress_bar(Ui& u, uint16_t w, uint16_t h, float frac, color fill, color track)
{
    frac = std::clamp(frac, 0.0f, 1.0f);
    u.element()
        .color(track)
        .radius(h / 2)
        .shape(shape::ROUNDED_RECTANGLE)
        .fixed(w, h)
        .row()
        .content([&](Ui& u2) {
            u2.element()
                .color(fill)
                .radius(h / 2)
                .shape(shape::ROUNDED_RECTANGLE)
                .fixed(static_cast<uint16_t>(std::max(1.0f, frac * w)), h);
        });
}

void tooltip(Ui& u, uint16_t x, uint16_t y, std::string_view title, color title_col,
             const std::string* body_lines, std::size_t body_count, uint16_t wrap_px)
{
    u.element()
        .floating(x, y)
        .color(theme().panel)
        .stroke(theme().stroke_hi, 2)
        .radius(10)
        .shape(shape::ROUNDED_RECTANGLE)
        .size({ fit(120), fit() })
        .pad({ 10, 10, 8, 8 })
        .gap(4)
        .column()
        .content([&](Ui& u2) {
            u2.text(title).color(title_col).font(22);
            for (std::size_t i = 0; i < body_count; ++i)
            {
                u2.text(body_lines[i])
                    .color(theme().text_dim)
                    .size({ fixed(wrap_px), fit() })   // fixed width -> shaper word-wraps to it
                    .font(18);
            }
        });
}

bool icon_cell(Ui& u, std::string_view id_name, std::string_view label, color border, uint16_t size,
               float cooldown, color fill)
{
    const id cid = make_id(id_name);
    const bool hot = u.interaction_state().is_hot(cid.hash);
    const bool focused = u.interaction_state().is_focused(cid.hash);
    const bool activated = u.interaction_state().is_pressed(cid.hash);

    // Hover/press lift: brighten the fill slightly.
    const color target = activated ? lerp_col(fill, theme().accent, 0.5f)
                                   : (hot || focused ? lerp_col(fill, border, 0.35f) : fill);
    const color anim_fill = u.motion().animate_color(cid.hash, target, { 0.12f, Curve::EASE_OUT });

    u.element(id_name)
        .color(anim_fill)
        .stroke(focused ? theme().accent_warm : border, focused ? 3 : 2)
        .radius(8)
        .shape(shape::ROUNDED_RECTANGLE)
        .fixed(size, size)
        .sweep(static_cast<uint8_t>(std::clamp(cooldown, 0.0f, 1.0f) * 255.0f))
        .pad(4)
        .align(alignment::CENTER)
        .column()
        .justify(justification::CENTER)
        .content([&](Ui& u2) { u2.text(label).color(theme().text).font(18); });

    if (activated)
        play_cue(Cue::Activate);
    return activated;
}

}  // namespace sandbox::ui
