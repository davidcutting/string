#pragma once

#include <cstdint>

#include <string/core/layout.hpp>

// UI interaction state (brief 12 M0b). Promoted out of the sandbox UI pass so the panel layer above
// it (dock drags, splitters, tab bars — all engine) has something to compile against.
//
// ARCHITECTURAL CONSTRAINT, LOAD-BEARING — do not relax without re-opening the brief-12 decision:
// `interaction` is a PLAIN VALUE TYPE. No back-pointer to the pass, no callbacks into the host, no
// ownership, no platform types. Brief 12 deliberately deferred "engine owns a ui::host" (option 3)
// in favour of "engine owns the STATE, the app owns the PRODUCER" (option 2); the deferral is only
// cheap while this stays a value. The moment it grows a `UIPass&` the deferred step becomes a
// rewrite instead of a move. Keeping it a value is also what makes L1 unit-testable with no device.
//
// The host fills `interaction_input` in ONE crossing function and calls the two resolvers below;
// everything else in the engine reads `interaction` and never talks to the host at all.
namespace string::ui
{

// Raw per-frame signals, supplied by the host. Deliberately platform-free: no Input, no window, no
// pass — the host has already de-edged buttons and resolved its own gamepad/stick conventions.
struct interaction_input
{
    float cursor_x = 0.0f;
    float cursor_y = 0.0f;
    // Cursor free (the UI takes clicks) vs captured (mouse-look drives the camera). The host owns
    // this decision; the resolvers only read it and may REQUEST a change (see wants_* below).
    bool ui_mode = false;
    bool primary_down = false;       // left button held this frame
    bool primary_pressed = false;    // press edge this frame
    bool primary_released = false;   // release edge this frame
    bool activate = false;           // gamepad south/A: activate the focused element
    int nav_x = 0;                   // -1 / 0 / +1 directional focus-nav intent, already de-edged
    int nav_y = 0;
    float dt = 0.0f;
    dimension screen{};
};

// Resolved interaction state — the value every engine UI layer reads.
struct interaction
{
    std::uint64_t hovered = 0;   // id hash under the cursor; 0 = none
    std::uint64_t focused = 0;   // id hash of the focused element; 0 = none
    std::uint64_t pressed = 0;   // id hash activated this frame (press edge / gamepad A); 0 = none

    // --- Drag + pointer capture (new in M0b; docking cannot work without it) ------------------
    // While `active` is non-zero it OWNS the pointer: hover stops being reassigned, so dragging a
    // splitter or a tab off its own box keeps tracking instead of snapping to whatever is under the
    // cursor. This is the primitive drag-to-dock, resize handles and tab tear-off are built from.
    std::uint64_t active = 0;
    float press_x = 0.0f;        // where the drag started, root space
    float press_y = 0.0f;
    float drag_x = 0.0f;         // live delta: cursor - press
    float drag_y = 0.0f;

    float cursor_x = 0.0f;
    float cursor_y = 0.0f;
    float dt = 0.0f;
    // Carried through from the input so layers above (panel clamping) don't need a second channel
    // for it. Still a plain value — `dimension` is POD.
    dimension screen{};

    // Mode REQUESTS back to the host (a click on empty UI space wants game mode; any focus-nav
    // intent wants UI mode). Requests, not actions: the host owns capture. Cleared each frame.
    bool wants_game_mode = false;
    bool wants_ui_mode = false;

    [[nodiscard]] constexpr bool is_hot(std::uint64_t id) const noexcept
    {
        return id != 0 && hovered == id;
    }
    [[nodiscard]] constexpr bool is_focused(std::uint64_t id) const noexcept
    {
        return id != 0 && focused == id;
    }
    [[nodiscard]] constexpr bool is_pressed(std::uint64_t id) const noexcept
    {
        return id != 0 && pressed == id;
    }
    [[nodiscard]] constexpr bool is_active(std::uint64_t id) const noexcept
    {
        return id != 0 && active == id;
    }
    [[nodiscard]] constexpr bool dragging() const noexcept { return active != 0; }
};

// Layer-aware hit test. `layout_builder::hit_test` picks the last node in pre-order that contains
// the point, which is painter order WITHIN a layer — but the renderer draws the whole overlay layer
// after the whole main layer, so an overlay panel must win against a later main-layer node. Without
// this a modal/floating panel is click-through wherever a main-layer node happens to sit on top of
// it in tree order. Returns the nearest id'd ancestor of the hit, matching hit_test's semantics
// (an id-less node — a button's label — is not interactive itself).
[[nodiscard]] const layout_node* hit_test_layered(const layout_builder& builder, uint16_t x,
                                                  uint16_t y) noexcept;

// Nearest focusable (non-zero id) node from `from` in direction (nav_x, nav_y), scored by
// directional distance plus a lateral penalty. `from` == 0 starts from the screen centre.
// Returns 0 when nothing lies in the requested half-plane.
[[nodiscard]] std::uint64_t nearest_focusable(const layout_builder& builder, std::uint64_t from,
                                              int nav_x, int nav_y, dimension screen) noexcept;

// --- The two resolvers. Split to match the frame's shape, which is load-bearing ----------------
// The author runs BETWEEN them: it must see this frame's press (so a button shows press feedback
// on the frame it is clicked) but hover/focus can only be resolved against a laid-out tree.

// Pre-author: resolve this frame's press/activation from the PREVIOUS frame's hover/focus, and
// advance drag state. Clears the mode requests.
void begin_interaction(interaction& state, const interaction_input& in) noexcept;

// Post-layout: hit-test for hover, resolve focus, run directional focus nav. `builder` must hold
// the tree the author just built.
void resolve_interaction(interaction& state, const interaction_input& in,
                         const layout_builder& builder) noexcept;

}  // namespace string::ui
