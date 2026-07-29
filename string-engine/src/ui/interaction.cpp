#include <string/ui/interaction.hpp>

#include <algorithm>
#include <cmath>

namespace string::ui
{

const layout_node* hit_test_layered(const layout_builder& builder, uint16_t x, uint16_t y) noexcept
{
    const std::span<const layout_node> nodes = builder.nodes();

    // The overlay flag is inherited by a subtree, but only the node that declares it carries the
    // bit — so resolve it by walking to the root, the same way the renderer's overlay_flags does.
    auto is_overlay = [&](const layout_node& n) {
        for (const layout_node* p = &n; p != nullptr;)
        {
            if (p->element.overlay) return true;
            p = p->is_root() ? nullptr : &nodes[p->parent];
        }
        return false;
    };

    // Last hit wins within a layer (painter order); the overlay layer beats the main layer outright
    // because the renderer draws all overlay content after all main content.
    const layout_node* hit = nullptr;
    bool hit_overlay = false;
    for (const layout_node& n : nodes)
    {
        if (!n.box.contains(x, y)) continue;
        const bool over = is_overlay(n);
        if (hit != nullptr && hit_overlay && !over) continue;   // main can never beat overlay
        hit = &n;
        hit_overlay = over;
    }

    // An id-less top node (e.g. a button's label painted over it) is not interactive itself —
    // resolve to the nearest ancestor carrying an id, matching layout_builder::hit_test.
    for (const layout_node* n = hit; n != nullptr;)
    {
        if (n->element.id.hash != 0) return n;
        n = n->is_root() ? nullptr : &nodes[n->parent];
    }
    return hit;
}

std::uint64_t nearest_focusable(const layout_builder& builder, std::uint64_t from, int nav_x,
                                int nav_y, dimension screen) noexcept
{
    if (nav_x == 0 && nav_y == 0) return 0;

    const layout_node* cur = from != 0 ? builder.find(from) : nullptr;
    const float cx = cur ? cur->box.x + cur->box.dimension.width * 0.5f : screen.width * 0.5f;
    const float cy = cur ? cur->box.y + cur->box.dimension.height * 0.5f : screen.height * 0.5f;

    std::uint64_t best = 0;
    float best_score = 1e18f;
    for (const layout_node& n : builder.nodes())
    {
        if (n.element.id.hash == 0 || n.element.id.hash == from)
            continue;  // only focusable (id'd) nodes; skip self
        const float nx = n.box.x + n.box.dimension.width * 0.5f;
        const float ny = n.box.y + n.box.dimension.height * 0.5f;
        const float dx = nx - cx;
        const float dy = ny - cy;
        // Must lie in the requested half-plane.
        const float along = dx * static_cast<float>(nav_x) + dy * static_cast<float>(nav_y);
        if (along <= 1.0f)
            continue;
        const float lateral = std::abs(dx * static_cast<float>(nav_y))
                            + std::abs(dy * static_cast<float>(nav_x));
        const float score = along + lateral * 2.0f;  // prefer aligned + close
        if (score < best_score)
        {
            best_score = score;
            best = n.element.id.hash;
        }
    }
    return best;
}

void begin_interaction(interaction& state, const interaction_input& in) noexcept
{
    state.wants_game_mode = false;
    state.wants_ui_mode = false;
    state.cursor_x = in.cursor_x;
    state.cursor_y = in.cursor_y;
    state.dt = in.dt;

    // A press edge in UI mode activates whatever was hovered as of last frame's layout; the gamepad
    // south button activates the focused element instead (controller-first).
    const bool click = in.ui_mode && in.primary_pressed;
    state.pressed = click ? state.hovered : (in.activate ? state.focused : 0);

    // --- Drag / pointer capture -----------------------------------------------------------------
    // A press on an id'd element takes the pointer; release drops it. While held, `active` is NOT
    // reassigned by hover, which is the whole point: a splitter dragged past its own box keeps
    // receiving the delta.
    if (click && state.hovered != 0)
    {
        state.active = state.hovered;
        state.press_x = in.cursor_x;
        state.press_y = in.cursor_y;
        state.drag_x = 0.0f;
        state.drag_y = 0.0f;
    }
    else if (state.active != 0)
    {
        // A release, or the button going down without us seeing the edge (focus loss, mode flip),
        // ends the drag. Holding keeps it live and updates the delta.
        if (in.primary_released || !in.primary_down || !in.ui_mode)
        {
            state.active = 0;
            state.drag_x = 0.0f;
            state.drag_y = 0.0f;
        }
        else
        {
            state.drag_x = in.cursor_x - state.press_x;
            state.drag_y = in.cursor_y - state.press_y;
        }
    }
}

void resolve_interaction(interaction& state, const interaction_input& in,
                         const layout_builder& builder) noexcept
{
    // While a drag owns the pointer, hover is pinned to the drag owner: reassigning it mid-drag is
    // what makes naive dock/resize implementations drop the handle the moment the cursor outruns it.
    if (state.active != 0)
    {
        state.hovered = state.active;
    }
    else
    {
        const layout_node* hit = hit_test_layered(
            builder, static_cast<std::uint16_t>(std::clamp(in.cursor_x, 0.0f, 65535.0f)),
            static_cast<std::uint16_t>(std::clamp(in.cursor_y, 0.0f, 65535.0f)));
        state.hovered = hit != nullptr ? hit->element.id.hash : 0;
    }

    if (!in.ui_mode)
    {
        state.focused = 0;  // game mode (mouse-look): nothing in the UI is focused
    }
    else if (in.primary_pressed)
    {
        // A click focuses the element under the cursor; a click on empty UI space falls through as
        // a game-mode request (so the UI gets first dibs on the click).
        if (state.hovered != 0)
        {
            state.focused = state.hovered;
        }
        else
        {
            state.wants_game_mode = true;
            state.focused = 0;
        }
    }

    if (in.nav_x != 0 || in.nav_y != 0)
    {
        state.wants_ui_mode = true;  // any nav intent means the player wants the UI
        const std::uint64_t best =
            nearest_focusable(builder, state.focused, in.nav_x, in.nav_y, in.screen);
        if (best != 0)
            state.focused = best;
    }
}

}  // namespace string::ui
