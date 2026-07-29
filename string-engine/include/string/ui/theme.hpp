#pragma once

#include <cstdint>

#include <string/core/layout.hpp>
#include <string/ui/motion.hpp>

// UI theme (brief 12 M0c, L3). The engine owns the TYPE; the app owns the VALUES.
//
// Brief 05 shipped the theme as 24 `constexpr color` constants in the sandbox, which cannot be
// inherited by an element tree or overridden per element — both of which the brief-12 conventions
// require ("central theme all elements inherit from, with per-element override"). So the theme
// becomes a value: one instance lives beside the Ui facade, every element reads its defaults from
// it, and `.color(...)` on an element overrides just that element.
//
// Game-specific vocabulary (item rarity, health/cast fills, hostile/friendly) deliberately does NOT
// live here — that is meaning, not interaction, and stays in the app's palette. The line is the
// same one brief 12 draws for widgets.
namespace string::ui
{

struct Theme
{
    // --- Surfaces -------------------------------------------------------------------------------
    color panel{ 26, 28, 42, 235 };
    color panel_alt{ 20, 22, 34, 235 };
    color stroke{ 70, 74, 100, 255 };
    color stroke_hi{ 137, 180, 250, 255 };

    // --- Content --------------------------------------------------------------------------------
    color text{ 205, 214, 244, 255 };
    color text_dim{ 140, 147, 175, 255 };
    color accent{ 137, 180, 250, 255 };
    color accent_warm{ 249, 226, 175, 255 };
    color good{ 166, 227, 161, 255 };
    color bad{ 243, 139, 168, 255 };

    // --- Metrics --------------------------------------------------------------------------------
    std::uint16_t pad = 12;
    std::uint16_t gap = 8;
    std::uint16_t radius = 12;
    std::uint16_t stroke_width = 2;

    std::uint16_t font_px = 20;        // body
    std::uint16_t font_px_small = 18;  // captions / dim rows
    std::uint16_t font_px_title = 26;  // panel titles

    // --- Motion ---------------------------------------------------------------------------------
    // Declared here so hover/press feel is themed too, not scattered through widget code.
    Transition hover{ 0.14f, Curve::EASE_OUT };
    Transition press{ 0.08f, Curve::EASE_OUT_BACK };
};

}  // namespace string::ui
