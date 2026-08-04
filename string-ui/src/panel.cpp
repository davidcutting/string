#include <string/ui/panel.hpp>

#include <algorithm>

namespace string::ui
{
namespace
{

// Clamp a moved panel so its HEADER stays wholly on screen.
//
// The header, not the panel: the body is allowed to hang off any edge (parking a panel mostly
// offscreen is a legitimate thing to do, and a panel taller than the viewport has nowhere else to
// go), but the strip you grab to drag it back must never be half-eaten by an edge. The previous rule
// — keep any 48px of the panel visible — let the header itself slide off, which on the lens meant
// the move strip, sitting ABOVE its rect, disappeared over the top of the screen while the rect it
// belonged to was still comfortably in view.
//
// Everything is solved for the RECT origin, since that is what is stored; the header offsets just
// shift the bounds.
void clamp_position(panel_rect& r, const panel_limits& limits, dimension screen) noexcept
{
    const float sw = static_cast<float>(screen.width);
    const float sh = static_cast<float>(screen.height);
    if (sw <= 0.0f || sh <= 0.0f) return;

    const panel_header& h = limits.header;

    // A header bigger than the viewport cannot be wholly visible, so the bounds invert. Clamping to
    // the inverted range instead is the graceful reading of the same rule: the header is then made
    // to SPAN the viewport rather than being flung to one side of it.
    const auto fit = [](float v, float lo, float hi) {
        return hi >= lo ? std::clamp(v, lo, hi) : std::clamp(v, hi, lo);
    };

    r.x = fit(r.x, -h.dx, sw - h.dx - (r.width + h.dwidth));
    r.y = fit(r.y, -h.dy, sh - h.dy - h.height);
}

}  // namespace

void update_panel(panel_state& state, const interaction& in, const panel_handles& handles,
                  const panel_limits& limits, dimension screen) noexcept
{
    const bool moving = handles.move != 0 && in.is_active(handles.move);
    const bool resizing = handles.resize != 0 && in.is_active(handles.resize);
    const std::uint64_t owner = moving ? handles.move : (resizing ? handles.resize : 0);

    if (owner == 0)
    {
        // No drag in flight. Re-clamp anyway so a resize of the window doesn't leave a panel
        // stranded off the new viewport.
        state.drag_owner = 0;
        clamp_position(state.rect, limits, screen);
        return;
    }

    // First frame of this drag: snapshot the rect the delta is measured against. See panel_state.
    if (state.drag_owner != owner)
    {
        state.drag_owner = owner;
        state.origin = state.rect;
    }

    if (moving)
    {
        state.rect.x = state.origin.x + in.drag_x;
        state.rect.y = state.origin.y + in.drag_y;
        clamp_position(state.rect, limits, screen);
        return;
    }

    // Resize from the bottom-right: the top-left corner is the anchor and does not move.
    state.rect.width = std::max(limits.min_width, state.origin.width + in.drag_x);
    state.rect.height = std::max(limits.min_height, state.origin.height + in.drag_y);

    // Don't let a resize push the panel's own body past the viewport; the min size still wins, so a
    // panel on a tiny viewport stays usable rather than collapsing to nothing.
    const float sw = static_cast<float>(screen.width);
    const float sh = static_cast<float>(screen.height);
    // Width is bounded by the HEADER's right edge, not the rect's, for the same reason the position
    // clamp is: widening the panel widens the header with it, and a header that grows off the edge
    // is exactly what the move clamp is there to prevent.
    const float head_right = limits.header.dx + limits.header.dwidth;
    if (sw > 0.0f)
        state.rect.width =
            std::max(limits.min_width, std::min(state.rect.width, sw - state.rect.x - head_right));
    if (sh > 0.0f) state.rect.height = std::max(limits.min_height, std::min(state.rect.height, sh - state.rect.y));
}

panel_state& PanelStore::panel(std::uint64_t id, panel_rect initial)
{
    panel_state& st = panels_[id];
    if (!st.placed)
    {
        st.rect = initial;
        st.placed = true;
        order_.push_back(id);   // a new panel starts on top
    }
    return st;
}

void PanelStore::bring_to_front(std::uint64_t id) noexcept
{
    const auto it = std::find(order_.begin(), order_.end(), id);
    if (it == order_.end() || it + 1 == order_.end())
        return;   // unknown, or already frontmost
    std::rotate(it, it + 1, order_.end());
}

std::size_t PanelStore::depth_of(std::uint64_t id) const noexcept
{
    const auto it = std::find(order_.begin(), order_.end(), id);
    return it == order_.end() ? 0 : static_cast<std::size_t>(it - order_.begin());
}


const panel_state* PanelStore::find(std::uint64_t id) const noexcept
{
    const auto it = panels_.find(id);
    return it == panels_.end() ? nullptr : &it->second;
}

}  // namespace string::ui
