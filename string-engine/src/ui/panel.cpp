#include <string/ui/panel.hpp>

#include <algorithm>

namespace string::ui
{
namespace
{

// Clamp a moved panel so a strip of it always remains reachable. Deliberately NOT "fully on
// screen": a panel wider than the viewport would otherwise be unmovable, and dragging a panel
// mostly off the edge is a legitimate way to park it.
void clamp_position(panel_rect& r, const panel_limits& limits, dimension screen) noexcept
{
    const float sw = static_cast<float>(screen.width);
    const float sh = static_cast<float>(screen.height);
    if (sw <= 0.0f || sh <= 0.0f) return;

    const float keep = std::min(limits.keep_visible, r.width);
    r.x = std::clamp(r.x, keep - r.width, sw - keep);

    // The top edge is clamped to 0 rather than to `keep`: the title bar is the only way to move a
    // panel, so letting it go above the top would strand the panel permanently.
    const float keep_y = std::min(limits.keep_visible, r.height);
    r.y = std::clamp(r.y, 0.0f, sh - keep_y);
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
    if (sw > 0.0f) state.rect.width = std::max(limits.min_width, std::min(state.rect.width, sw - state.rect.x));
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
