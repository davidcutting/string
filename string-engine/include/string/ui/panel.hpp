#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <string/core/layout.hpp>
#include <string/ui/interaction.hpp>

// Floating panels (brief 12 M1, L5) — move + resize. The first piece of engine-side panel
// machinery, and the first real consumer of the pointer capture added in M0b.
//
// WHY THIS STATE LIVES OUTSIDE THE LAYOUT TREE: the UI is immediate-mode, so the layout tree is
// thrown away and rebuilt every frame. "Where is this panel, how big is it" must survive that, so
// it lives in a store keyed by the panel's stable id hash — exactly like `Motion` keeps per-element
// animation state. That is the whole reason the store exists; it is NOT about serialising anything
// to disk (deferred out of this brief).
//
// The dock tree (splits, slots, tab groups) is deliberately NOT here yet. A floating panel needs
// nothing but a rect, and building the tree before anything drags would be designing against
// guesses.
namespace string::ui
{

struct panel_rect
{
    // float, not uint16: a drag accumulates sub-pixel cursor deltas, and rounding to the layout's
    // integer coordinates on every frame would make a slow drag stutter or drift.
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    [[nodiscard]] constexpr float right() const noexcept { return x + width; }
    [[nodiscard]] constexpr float bottom() const noexcept { return y + height; }
};

// Lower bound on a panel's size, and the slack allowed when clamping it against the screen.
struct panel_limits
{
    float min_width = 160.0f;
    float min_height = 96.0f;
    // How much of the panel must remain on screen. Keeping a strip visible (rather than the whole
    // panel) is what lets a panel be pushed mostly off the edge and still be draggable back.
    float keep_visible = 48.0f;
};

// The two draggable handles a floating panel exposes. Both are ordinary id'd elements, so they
// hover, focus and capture the pointer through the same path as every other element.
struct panel_handles
{
    std::uint64_t move = 0;    // the title bar
    std::uint64_t resize = 0;  // the bottom-right grip
};

struct panel_state
{
    panel_rect rect{};
    bool placed = false;  // false until an initial rect has been applied

    // THE DRAG ORIGIN, and why it is stored rather than accumulated: `interaction::drag_x/y` is an
    // ABSOLUTE delta from the press point, not a per-frame increment. So the rect is recomputed as
    // origin + delta every frame instead of being nudged. That makes clamping idempotent — a panel
    // dragged hard into a screen edge and back again returns to where the cursor says it should be,
    // where an accumulate-and-clamp implementation would have silently eaten the clamped motion.
    std::uint64_t drag_owner = 0;
    panel_rect origin{};
};

// Applies this frame's drag to `state.rect`. Pure logic: no builder, no device, no host — which is
// what makes the move/resize/clamp behaviour unit-testable per the brief's loose-coupling rule.
//
// Call once per panel per frame, BEFORE authoring it: `begin_interaction` has already advanced the
// drag, so the rect this produces is the one the panel should be laid out at this frame (no
// one-frame lag between cursor and panel).
void update_panel(panel_state& state, const interaction& in, const panel_handles& handles,
                  const panel_limits& limits, dimension screen) noexcept;

// Per-panel persistent state, keyed by the panel's stable id hash.
class PanelStore
{
public:
    // Returns the panel's state, creating it on first use. The initial rect is applied ONLY on
    // creation — an author may compute it however it likes each frame without fighting the drag.
    panel_state& panel(std::uint64_t id, panel_rect initial);

    [[nodiscard]] const panel_state* find(std::uint64_t id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return panels_.size(); }
    void clear() noexcept { panels_.clear(); order_.clear(); }

    // --- Front-to-back order -----------------------------------------------------------------
    // Back-to-front: order_[0] is the bottom panel, order_.back() is the frontmost. A panel is
    // appended on creation, so a newly authored panel starts on top.
    [[nodiscard]] std::span<const std::uint64_t> order() const noexcept { return order_; }

    // Moves `id` to the front. No-op if unknown or already frontmost.
    void bring_to_front(std::uint64_t id) noexcept;

    // Where `id` sits, 0 = backmost. Returns 0 for unknown ids.
    [[nodiscard]] std::size_t depth_of(std::uint64_t id) const noexcept;


private:
    std::unordered_map<std::uint64_t, panel_state> panels_;
    std::vector<std::uint64_t> order_;
};

}  // namespace string::ui
