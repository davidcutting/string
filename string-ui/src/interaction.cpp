#include <string/ui/interaction.hpp>

#include <algorithm>
#include <cmath>

namespace string::ui
{

const layout_node* hit_test_layered(const layout_builder& builder, uint16_t x, uint16_t y) noexcept
{
    const std::span<const layout_node> nodes = builder.nodes();

    // `overlay` and `z` are both inherited by a subtree, but only the node that declares them
    // carries the value — so resolve by walking to the root, the same way the renderer resolves its
    // draw-batching key. THIS MUST MATCH THE RENDERER'S KEY: draw order is (overlay, z, tree order),
    // so if the hit test used anything else the panel you see on top would not be the one that gets
    // the click.
    auto layer_key = [&](const layout_node& n) {
        unsigned layer = 0;
        unsigned z = 0;
        for (const layout_node* p = &n; p != nullptr;)
        {
            // A subtree cannot sink below the layer it was placed on, so the MAX along the ancestry
            // wins — the same rule the single overlay bit had, generalised to an ordered list.
            layer = std::max(layer, static_cast<unsigned>(p->element.layer));
            if (z == 0) z = p->element.z;   // nearest ancestor-or-self that declares one wins
            p = p->is_root() ? nullptr : &nodes[p->parent];
        }
        return (layer << 8) | z;
    };

    // A node is only hittable where its ancestors' clip regions allow. Without this, content
    // scrolled out of a clipped view stays clickable — invisible but still taking presses, which is
    // the worst kind of bug to diagnose because there is nothing on screen to point at.
    auto visible_at = [&](const layout_node& n) {
        for (const layout_node* p = &n; p != nullptr;)
        {
            if (p->element.clip && !builder.screen_box(*p).contains(x, y)) return false;
            p = p->is_root() ? nullptr : &nodes[p->parent];
        }
        return true;
    };

    // Last hit wins within a layer (painter order); a higher layer beats a lower one outright,
    // because the renderer draws each layer's shapes AND text before moving to the next.
    const layout_node* hit = nullptr;
    unsigned hit_key = 0;
    for (const layout_node& n : nodes)
    {
        if (!builder.screen_box(n).contains(x, y)) continue;
        const unsigned key = layer_key(n);
        if (hit != nullptr && key < hit_key) continue;   // a lower layer can never win
        if (!visible_at(n)) continue;
        hit = &n;
        hit_key = key;
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

namespace
{

// The innermost `scope`/`modal` ancestor-or-self of a node, or `none` when it is in no scope.
// Confinement is defined by this: two nodes are navigable to each other only if they answer the
// same scope.
std::uint32_t scope_of(std::span<const layout_node> nodes, const layout_node* n) noexcept
{
    for (const layout_node* p = n; p != nullptr;)
    {
        if (p->element.scope || p->element.modal)
            return static_cast<std::uint32_t>(p - nodes.data());
        p = p->is_root() ? nullptr : &nodes[p->parent];
    }
    return layout_node::none;
}

}  // namespace

std::uint64_t nearest_focusable(const layout_builder& builder, std::uint64_t from, int nav_x,
                                int nav_y, dimension screen) noexcept
{
    if (nav_x == 0 && nav_y == 0) return 0;

    const layout_node* cur = from != 0 ? builder.find(from) : nullptr;
    // Nav is CONFINED to the focused element's innermost scope. Without this the stick walks out of
    // an open dialog into whatever sits behind it, which looks like the dialog failing to be modal.
    // With no focus, or focus outside any scope, the whole tree is in play — which is the correct
    // reading of "nothing has claimed the focus".
    const std::uint32_t confine = scope_of(builder.nodes(), cur);
    // SCREEN space on both sides: nav compares boxes across surfaces, which only share a frame of
    // reference once their placements are applied.
    const bounding_box cur_box = cur ? builder.screen_box(*cur) : bounding_box{};
    const float cx = cur ? cur_box.x + cur_box.dimension.width * 0.5f : screen.width * 0.5f;
    const float cy = cur ? cur_box.y + cur_box.dimension.height * 0.5f : screen.height * 0.5f;

    std::uint64_t best = 0;
    float best_score = 1e18f;
    for (const layout_node& n : builder.nodes())
    {
        // DECLARED focusable, not merely id'd. See element::focusable — an id is what a drag handle
        // or a scroll track needs to be addressable, and nav landing on one of those is nonsense.
        if (!n.element.focusable || n.element.id.hash == 0 || n.element.id.hash == from)
            continue;
        // A zero-area node cannot be seen, so it cannot be a sensible place for focus to land.
        // Collapsed content and empty rows produce these, and they are invisible traps otherwise.
        if (n.box.dimension.width == 0 || n.box.dimension.height == 0)
            continue;
        if (scope_of(builder.nodes(), &n) != confine)
            continue;   // a different scope: out of reach until focus moves there deliberately
        const bounding_box nb = builder.screen_box(n);
        const float nx = nb.x + nb.dimension.width * 0.5f;
        const float ny = nb.y + nb.dimension.height * 0.5f;
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

// Where `a` lands this frame: from the focused element up its ancestors to the first that declares
// it handles `a`, stopping at a modal scope. Returns 0 for nobody.
//
// FALLBACK WHEN NOTHING IS FOCUSED: the topmost node that handles the action, by the same layer key
// the renderer and the hit test use. That is what makes Escape close the frontmost surface without
// the user having first tabbed into it — which is how everyone expects Escape to behave, and it
// falls out of the ordering that already exists rather than needing a rule of its own.
namespace
{
std::uint64_t route_action(const layout_builder& builder, std::uint64_t focused, ui_action a) noexcept
{
    const std::span<const layout_node> nodes = builder.nodes();
    const std::uint32_t bit = action_bit(a);

    if (const layout_node* from = focused != 0 ? builder.find(focused) : nullptr; from != nullptr)
    {
        for (const layout_node* n = from; n != nullptr;)
        {
            if ((n->element.actions & bit) != 0 && n->element.id.hash != 0)
                return n->element.id.hash;
            // A modal swallows what it does not handle. Letting it bubble would mean the surface
            // BEHIND a dialog acting on input the user aimed at the dialog.
            if (n->element.modal)
                return 0;
            n = n->is_root() ? nullptr : &nodes[n->parent];
        }
        return 0;
    }

    std::uint64_t best = 0;
    unsigned best_key = 0;
    for (const layout_node& n : nodes)
    {
        if ((n.element.actions & bit) == 0 || n.element.id.hash == 0) continue;
        unsigned layer = 0, z = 0;
        for (const layout_node* p = &n; p != nullptr;)
        {
            layer = std::max(layer, static_cast<unsigned>(p->element.layer));
            if (z == 0) z = p->element.z;
            p = p->is_root() ? nullptr : &nodes[p->parent];
        }
        const unsigned key = (layer << 8) | z;
        if (best == 0 || key >= best_key)   // >= so later tree order wins a tie, as in painter order
        {
            best = n.element.id.hash;
            best_key = key;
        }
    }
    return best;
}
}  // namespace

void begin_interaction(interaction& state, const interaction_input& in) noexcept
{
    state.wants_game_mode = false;
    state.wants_ui_mode = false;
    state.cursor_x = in.cursor_x;
    state.cursor_y = in.cursor_y;
    state.dt = in.dt;
    state.screen = in.screen;
    state.typed_text = in.typed_text;
    state.backspace = in.backspace;
    state.del = in.del;
    state.caret_left = in.caret_left;
    state.caret_right = in.caret_right;
    state.caret_home = in.caret_home;
    state.caret_end = in.caret_end;
    state.select_mod = in.select_mod;
    state.word_mod = in.word_mod;
    state.copy = in.copy;
    state.cut = in.cut;
    state.paste = in.paste;
    state.select_all = in.select_all;
    state.clipboard = in.clipboard;
    state.scroll_y = in.scroll_y;

    // A press edge in UI mode activates whatever was hovered as of last frame's layout; the gamepad
    // south button activates the focused element instead (controller-first).
    const bool click = in.ui_mode && in.primary_pressed;
    // The gamepad path is now an ACTION rather than a hardcoded button: `activate` is bound by the
    // host like anything else, so a player can move it off the south button.
    const bool activate = (in.actions & action_bit(ui_action::activate)) != 0;
    state.pressed = click ? state.hovered : (activate ? state.focused : 0);

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
        // The drag owner may be nowhere near the cursor by now, so look it up by id rather than by
        // position — that is the whole point of pointer capture.
        const layout_node* owner = builder.find(state.active);
        // SCREEN space: `hovered_box` is PUBLISHED — widgets anchor to it and compare it against
        // cursor coordinates, neither of which knows what surface the node lives on.
        if (owner != nullptr) state.hovered_box = builder.screen_box(*owner);
    }
    const layout_node* hit = nullptr;
    if (state.active == 0)
    {
        hit = hit_test_layered(
            builder, static_cast<std::uint16_t>(std::clamp(in.cursor_x, 0.0f, 65535.0f)),
            static_cast<std::uint16_t>(std::clamp(in.cursor_y, 0.0f, 65535.0f)));
        state.hovered = hit != nullptr ? hit->element.id.hash : 0;
        state.hovered_box = hit != nullptr ? builder.screen_box(*hit) : bounding_box{};
    }

    // --- Wheel routing. Walk from the hit node to the nearest ancestor-or-self that declared
    // `element.wheel`, so the INNERMOST scrollable under the cursor wins.
    //
    // Resolved EVERY frame, not only when notches arrive, and that is load-bearing: the notches are
    // read by `begin_interaction` at the top of a frame, while the tree they must be resolved against
    // is only laid out at the end of the previous one. Gating this on `in.scroll_y` looks like an
    // optimisation and is actually an off-by-one-frame — the wheel would need two consecutive
    // notched frames to move anything. This is `hovered`'s staleness, and no more.
    //
    // The drag owner takes the wheel while a drag is in flight, for the same reason it takes hover:
    // a gesture that has captured the pointer should not have input stolen by whatever it flew over.
    state.wheel = 0;
    {
        const std::span<const layout_node> nodes = builder.nodes();
        const layout_node* from = state.active != 0 ? builder.find(state.active) : hit;
        for (const layout_node* n = from; n != nullptr;)
        {
            if (n->element.wheel && n->element.id.hash != 0)
            {
                state.wheel = n->element.id.hash;
                break;
            }
            n = n->is_root() ? nullptr : &nodes[n->parent];
        }
    }

    if (!in.ui_mode)
    {
        state.focused = 0;  // game mode (mouse-look): nothing in the UI is focused
    }
    else if (in.primary_pressed)
    {
        // A click focuses the element under the cursor; a click on empty UI space falls through as
        // a game-mode request (so the UI gets first dibs on the click).
        //
        // Only a FOCUSABLE element takes focus. Clicking pointer-driven chrome — a title bar, a
        // splitter, a scroll track — clears it instead, which is both what every desktop UI does and
        // the only reading consistent with nav: an element nav cannot reach should not be somewhere
        // focus can get stuck, or the next stick push would be measured from a resize grip.
        const layout_node* target = state.hovered != 0 ? builder.find(state.hovered) : nullptr;
        if (target != nullptr && target->element.focusable)
        {
            state.focused = state.hovered;
        }
        else
        {
            if (state.hovered == 0) state.wants_game_mode = true;
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

    // --- Route this frame's actions. LAST, so an action lands on the focus AFTER nav has moved it —
    // press a direction and activate on the same frame and you activate what you navigated to, which
    // is the only reading that is not a surprise.
    //
    // Cleared unconditionally: a target is an EVENT for one frame, and a stale one would re-fire a
    // dialog's cancel every frame until something else claimed it.
    for (std::size_t i = 0; i < static_cast<std::size_t>(ui_action::count); ++i)
    {
        const auto a = static_cast<ui_action>(i);
        state.action_target[i] =
            (in.actions & action_bit(a)) != 0 ? route_action(builder, state.focused, a) : 0;
    }
}

}  // namespace string::ui
