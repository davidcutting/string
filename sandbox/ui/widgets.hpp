#pragma once

#include <cstdint>
#include <string_view>

#include <string/core/layout.hpp>

#include "passes/ui_pass.hpp"
#include "ui/motion.hpp"
#include "ui/theme.hpp"

namespace sandbox::ui
{

// Immediate-mode widget kit (brief 05, milestone 3). Widgets are free functions over the layout
// builder + the interaction context + the motion table — no retained-widget machinery leaks into the
// API (a screen is authored top-to-bottom each frame). They ride on the motion system for hover
// glide / press feedback and on the dynamic text atlas for labels. Stable ids (make_id) key both
// hit-testing/focus and the animation state.

// Interaction snapshot a widget needs: hovered/focused/pressed element ids + delta_time, lifted out
// of UIPass::UiContext so widgets don't depend on the pass. Build one per frame from the context.
struct Interaction
{
    std::uint64_t hovered = 0;
    std::uint64_t focused = 0;
    std::uint64_t pressed = 0;
    float dt = 0.0f;

    static Interaction from(const UIPass::UiContext& ctx)
    {
        return { ctx.hovered, ctx.focused, ctx.pressed, ctx.delta_time };
    }
    bool is_hot(std::uint64_t id) const { return id != 0 && hovered == id; }
    bool is_focused(std::uint64_t id) const { return id != 0 && focused == id; }
    bool is_pressed(std::uint64_t id) const { return id != 0 && pressed == id; }
};

// Open a rounded panel container (caller adds children then calls layout_builder::end()). Soft flat
// styling. `fill`/`stroke` default to the theme panel.
void begin_panel(string::layout_builder& b, string::color fill = col::panel,
                 string::color stroke = col::stroke, uint16_t radius = 12, uint16_t pad = 12,
                 uint16_t gap = 8, string::direction dir = string::direction::VERTICAL);

// A text button with hover glide + press-scale feedback (motion-driven) and focus-visible ring.
// Returns true if activated this frame (pressed edge). `id_name` must be stable across frames.
bool button(string::layout_builder& b, Motion& m, const Interaction& it, std::string_view id_name,
            std::string_view label, uint16_t font_px = 22, uint16_t min_w = 0);

// A labelled horizontal progress/resource bar (health, cast, XP). `frac` 0..1. Rounded track + fill.
void progress_bar(string::layout_builder& b, uint16_t w, uint16_t h, float frac,
                  string::color fill, string::color track = col::hp_bg);

// A floating tooltip anchored at (x,y): a rounded panel with a title line + body lines. Sized by its
// text (layout-driven). Body may wrap to `wrap_px`.
void tooltip(string::layout_builder& b, uint16_t x, uint16_t y, std::string_view title,
             string::color title_col, const std::string* body_lines, std::size_t body_count,
             uint16_t wrap_px = 240);

// A square icon cell (rarity-bordered) for an inventory/action grid. `label` (short) is centred;
// `count` (>=0) draws a small stack-count corner number handled by the caller if needed. Hover/focus
// styling via `it` + `id_name`. Returns true if activated. `cooldown` 0..1 draws a radial sweep mask.
bool icon_cell(string::layout_builder& b, Motion& m, const Interaction& it, std::string_view id_name,
               std::string_view label, string::color border, uint16_t size = 56,
               float cooldown = 0.0f, string::color fill = col::panel_alt);

}  // namespace sandbox::ui
