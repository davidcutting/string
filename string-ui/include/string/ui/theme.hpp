#pragma once

#include <cstdint>

#include <string/ui/layout.hpp>
#include <string/ui/motion.hpp>

// UI theme (brief 12 M0c, L3). The engine owns the TYPE; the app owns the VALUES.
//
// A VALUE, not a set of constants, because the brief-12 conventions require a central theme every
// element inherits with per-element override — which constants cannot express. One instance lives
// beside the Ui facade, every element reads its defaults from it, and `.color(...)` on an element
// overrides just that element.
//
// Game-specific vocabulary (item rarity, health/cast fills, hostile/friendly) deliberately does NOT
// live here — that is meaning, not interaction, and stays in the app's palette. The line is the
// same one brief 12 draws for widgets.
namespace string::ui
{

// The interaction state an element is drawn in. Passed as one value rather than four bools at each
// call site, so the mapping from state to colour lives in ONE place (see Theme::surface/outline
// below) instead of being re-spelled as a ternary in every widget.
//
// Designated-initialiser friendly on purpose: `th.surface({ .hot = hot, .selected = sel })` reads as
// a statement of state, which is the thing a widget actually knows.
struct visual_state
{
    bool hot = false;       // pointer is over it
    bool active = false;    // pressed, or a drag is in flight on it
    bool focused = false;   // holds keyboard/gamepad focus
    bool selected = false;  // the chosen one of a set (a table row, a tab, a combo option)
};

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

    // --- State -> colour ---------------------------------------------------------------------
    //
    // These exist for the same reason the Transitions above do: "what does hover look like" is a
    // THEME question, and it was previously answered by ~13 hand-written ternaries scattered across
    // the widgets — so changing hover feel meant finding all of them and getting each right.
    //
    // Only the mappings that were already unanimous are here. Several widgets deviate deliberately
    // (a menu row goes to `accent` on hover; a tab keeps its resting fill and signals selection with
    // its stroke), and those keep their own expressions rather than being bent into a shared one.

    // Fill for a selectable/hoverable row, option or header.
    [[nodiscard]] constexpr color surface(visual_state s) const noexcept
    {
        if (s.selected) return accent;
        return (s.hot || s.active) ? panel_alt : panel;
    }

    // Border for an interactive control. Focus outranks hover: focus persists and is the state the
    // keyboard acts on, so it must remain readable while the pointer wanders elsewhere.
    [[nodiscard]] constexpr color outline(visual_state s) const noexcept
    {
        if (s.focused) return accent;
        return (s.hot || s.active) ? stroke_hi : stroke;
    }
};

// --- The process-wide default palette ---------------------------------------------------------
//
// The kit owns the theme TYPE; the APP owns the VALUES — that split is deliberate and unchanged.
// What this adds is a place to PUT the app's chosen instance so that independent libraries (the
// debug surfaces, the game screens) can read one palette without depending on each other, or on
// the app. The app calls set_theme() once at startup; everything else reads theme().
//
// Same shape as the engine's GpuProfiler/GraphIntrospect globals: a non-owning process-global set
// once during startup and read-only thereafter. Until it is set, theme() returns the kit defaults,
// so a headless test or a tool that never sets one still renders.
void set_theme(const Theme& t);
[[nodiscard]] const Theme& theme();

}  // namespace string::ui
