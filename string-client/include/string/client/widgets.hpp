#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <string/ui/layout.hpp>
#include <string/ui/interaction.hpp>
#include <string/ui/motion.hpp>
#include <string/ui/ui.hpp>

#include <string/client/theme.hpp>

namespace string::client
{

// The engine owns the UI vocabulary as of brief 12 M0b (L1 interaction + L2 motion were promoted to
// `::string::ui`); the app spells it unqualified. These are adoptions, not wrappers — same types, no
// indirection, so the app and the engine can never drift out of sync.
using Motion = ::string::ui::Motion;
using Curve = ::string::ui::Curve;
using Transition = ::string::ui::Transition;
using Interaction = ::string::ui::interaction;
using Ui = ::string::ui::Ui;
using Element = ::string::ui::Element;

// Immediate-mode widget kit (brief 05, milestone 3). Widgets are free functions over the layout
// builder + the interaction context + the motion table — no retained-widget machinery leaks into the
// API (a screen is authored top-to-bottom each frame). They ride on the motion system for hover
// glide / press feedback and on the dynamic text atlas for labels. Stable ids (make_id) key both
// hit-testing/focus and the animation state.

// Open a rounded panel container (caller adds children then calls layout_builder::end()). Soft flat
// styling. `fill`/`stroke` default to the theme panel.
void begin_panel(Ui& u, ::string::color fill = theme().panel,
                 ::string::color stroke = theme().stroke, uint16_t radius = 12, uint16_t pad = 12,
                 uint16_t gap = 8, ::string::direction dir = ::string::direction::VERTICAL);

// A text button with hover glide + press-scale feedback (motion-driven) and focus-visible ring.
// Returns true if activated this frame (pressed edge). `id_name` must be stable across frames.
bool button(Ui& u, std::string_view id_name,
            std::string_view label, uint16_t font_px = 22, uint16_t min_w = 0);

// A labelled horizontal progress/resource bar (health, cast, XP). `frac` 0..1. Rounded track + fill.
void progress_bar(Ui& u, uint16_t w, uint16_t h, float frac,
                  ::string::color fill, ::string::color track = col::hp_bg);

// A floating tooltip anchored at (x,y): a rounded panel with a title line + body lines. Sized by its
// text (layout-driven). Body may wrap to `wrap_px`.
void tooltip(Ui& u, uint16_t x, uint16_t y, std::string_view title,
             ::string::color title_col, const std::string* body_lines, std::size_t body_count,
             uint16_t wrap_px = 240);

// A square icon cell (rarity-bordered) for an inventory/action grid. `label` (short) is centred;
// `count` (>=0) draws a small stack-count corner number handled by the caller if needed. Hover/focus
// styling via `it` + `id_name`. Returns true if activated. `cooldown` 0..1 draws a radial sweep mask.
bool icon_cell(Ui& u, std::string_view id_name,
               std::string_view label, ::string::color border, uint16_t size = 56,
               float cooldown = 0.0f, ::string::color fill = theme().panel_alt);

}  // namespace string::client
