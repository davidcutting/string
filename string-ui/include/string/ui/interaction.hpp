#pragma once

#include <cstdint>
#include <string_view>

#include <string/ui/layout.hpp>

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
    // `ui_action` bits that fired THIS frame (see action_bit). The host resolves these from its
    // remappable bindings, so which key means "cancel" is a player setting and the kit never learns
    // what a key is. Edges, already de-edged by the host — an action is an event, not a state.
    std::uint32_t actions = 0;
    int nav_x = 0;                   // -1 / 0 / +1 directional focus-nav intent, already de-edged
    int nav_y = 0;
    float dt = 0.0f;
    dimension screen{};

    // --- Text entry ------------------------------------------------------------------------------
    // Composed character input for THIS frame as UTF-8 (honours keyboard layout, shift and IME) —
    // distinct from key state, and empty on most frames. A view, not a string: it points at the
    // host's per-frame buffer and is consumed the same frame, which keeps `interaction` a trivially
    // copyable value.
    std::string_view typed_text{};

    // --- Caret / selection editing -----------------------------------------------------------
    // Editing keys, NOT actions. Brief 17 keeps text out of the action system (you do not rebind
    // 'e' to mean 'e'), and a caret key is text editing by the same argument — nobody rebinds Home.
    //
    // Each is an edge **including auto-repeat**, resolved by the host: holding Left must walk the
    // caret, and the repeat delay and rate are an OS accessibility setting rather than something a
    // widget may invent.
    bool backspace = false;
    bool del = false;            // forward delete
    bool caret_left = false;
    bool caret_right = false;
    bool caret_home = false;
    bool caret_end = false;
    // Modifiers: extend the selection rather than move, and move by word rather than character.
    bool select_mod = false;     // shift
    bool word_mod = false;       // ctrl
    // Clipboard chords, resolved by the host — WHICH chord means copy is a platform convention
    // (Cmd on macOS), so the kit receives intent and never learns what Ctrl is.
    bool copy = false;
    bool cut = false;
    bool paste = false;
    bool select_all = false;
    // The system clipboard as the host last saw it. A view, consumed the same frame, so
    // `interaction_input` stays trivially copyable.
    std::string_view clipboard{};

    // Wheel notches this frame. An EVENT, not a state: it is whatever arrived since the last frame
    // and is zero on most of them.
    float scroll_y = 0.0f;
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

    // The box of whatever `hovered` refers to (so, while a drag is in flight, of the drag owner).
    //
    // This is what lets a control map a cursor position onto ITSELF — "you pressed 80% along this
    // track, so the value is 80%" — which is the one thing a slider needs and a drag-value does not.
    // It is free: `resolve_interaction` already hit-tests the tree and finds this node, and until now
    // simply discarded the box and kept the id. Keeping it costs four numbers and no extra work.
    //
    // Deliberately NOT a general id->box cache. That would be a map, a post-layout walk and a frame
    // of staleness for every element, to serve the handful that are pointer-driven. A widget that
    // needs its rect CONTINUOUSLY rather than while-pressed (a table sizing its virtual viewport, a
    // graph canvas) should observe its own, the way `Workspace` does.
    bounding_box hovered_box{};

    float cursor_x = 0.0f;
    float cursor_y = 0.0f;
    float dt = 0.0f;
    // Carried through from the input so layers above (panel clamping) don't need a second channel
    // for it. Still a plain value — `dimension` is POD.
    dimension screen{};

    // --- Routed actions ---------------------------------------------------------------------
    // For each `ui_action`, the id it was ROUTED to this frame (0 = nobody). Resolved post-layout by
    // walking up from the focused element to the first ancestor-or-self that declares it handles
    // that action, stopping at a modal scope.
    //
    // ARBITRATION LIVES HERE, not in the widgets, for exactly the reason it does for the wheel: with
    // a text field inside a dialog inside a panel, precisely one of them must act on Enter, and
    // "whoever checks first" is not a rule — it is a bug that depends on authoring order. Resolving
    // to a single target makes consumption structural: there is no second claimant to lose a race to.
    //
    // A fixed array rather than a map because the vocabulary is closed and tiny, and because
    // `interaction` must stay a plain, trivially copyable value (see the note at the top).
    std::uint64_t action_target[static_cast<std::size_t>(ui_action::count)]{};

    // Text entry for this frame, forwarded verbatim. Only the FOCUSED widget should consume it.
    //
    // Text is NOT an action and never will be: you do not rebind 'e' to mean 'e'. Backspace stays
    // here with it as an editing key rather than becoming a bindable action, for the same reason.
    std::string_view typed_text{};
    bool backspace = false;
    bool del = false;
    bool caret_left = false;
    bool caret_right = false;
    bool caret_home = false;
    bool caret_end = false;
    bool select_mod = false;
    bool word_mod = false;
    bool copy = false;
    bool cut = false;
    bool paste = false;
    bool select_all = false;
    std::string_view clipboard{};

    // Wheel notches this frame. The widget UNDER THE CURSOR should consume it — unlike text, which
    // goes to whatever is focused. Scrolling follows the pointer; typing follows focus.
    float scroll_y = 0.0f;
    // Which container the wheel belongs to: the innermost ancestor-or-self of the hit node that
    // declared `element.wheel`. Resolved by the hit test because only the tree knows the ancestry —
    // the cursor is usually over a ROW, not over the list that scrolls.
    //
    // Arbitration lives here rather than in each widget for the reason focus does: with a list inside
    // a panel inside a graph canvas, exactly one must move, and "whoever checks first" is not a rule.
    std::uint64_t wheel = 0;

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
    // Wheel notches for `id`, or 0 if the wheel went somewhere else. The one call a scrollable
    // widget makes — it never has to ask whether it is hovered, which would be the wrong question
    // anyway (its rows are what is hovered).
    [[nodiscard]] constexpr float wheel_for(std::uint64_t id) const noexcept
    {
        return id != 0 && wheel == id ? scroll_y : 0.0f;
    }

    // Did `id` receive `a` this frame? The one call a widget makes — it never has to ask whether it
    // is focused, which would be the wrong question anyway (a dialog handling `cancel` is usually
    // not the focused element; one of its children is).
    [[nodiscard]] constexpr bool took(std::uint64_t id, ui_action a) const noexcept
    {
        return id != 0 && action_target[static_cast<std::size_t>(a)] == id;
    }
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
