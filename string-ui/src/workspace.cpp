#include <string/ui/workspace.hpp>

#include <algorithm>
#include <cmath>

namespace string::ui
{
namespace
{

[[nodiscard]] placement side_placement(side w, std::uint64_t anchor, axis_sizing s) noexcept
{
    placement p;
    p.mode = placement::kind::Side;
    p.where = w;
    p.anchor = anchor;
    p.sizing = s;
    return p;
}

[[nodiscard]] direction axis_of(side w) noexcept
{
    return (w == side::Left || w == side::Right) ? direction::HORIZONTAL : direction::VERTICAL;
}

// Does the new region come BEFORE the existing one along the split axis?
[[nodiscard]] bool leading(side w) noexcept
{
    return w == side::Left || w == side::Top;
}

}  // namespace

placement left(axis_sizing s) noexcept { return side_placement(side::Left, 0, s); }
placement right(axis_sizing s) noexcept { return side_placement(side::Right, 0, s); }
placement top(axis_sizing s) noexcept { return side_placement(side::Top, 0, s); }
placement bottom(axis_sizing s) noexcept { return side_placement(side::Bottom, 0, s); }

placement left_of(std::uint64_t card, axis_sizing s) noexcept
{
    return side_placement(side::Left, card, s);
}
placement right_of(std::uint64_t card, axis_sizing s) noexcept
{
    return side_placement(side::Right, card, s);
}
placement top_of(std::uint64_t card, axis_sizing s) noexcept
{
    return side_placement(side::Top, card, s);
}
placement bottom_of(std::uint64_t card, axis_sizing s) noexcept
{
    return side_placement(side::Bottom, card, s);
}

placement tab_with(std::uint64_t card) noexcept
{
    placement p;
    p.mode = placement::kind::Tab;
    p.anchor = card;
    return p;
}

placement floating_at(panel_rect r) noexcept
{
    placement p;
    p.mode = placement::kind::Floating;
    p.rect = r;
    return p;
}

// --- Node storage --------------------------------------------------------------------------------

node_index Workspace::alloc(workspace_node n)
{
    n.uid = next_uid_++;   // never reused, unlike the slot index
    if (!free_.empty())
    {
        const node_index i = free_.back();
        free_.pop_back();
        nodes_[i] = std::move(n);
        dead_[i] = 0;
        return i;
    }
    nodes_.push_back(std::move(n));
    dead_.push_back(0);
    return static_cast<node_index>(nodes_.size() - 1);
}

void Workspace::observe(const layout_builder& b) noexcept
{
    for (std::size_t i = 0; i < nodes_.size(); ++i)
    {
        if (dead_[i] || nodes_[i].element_hash == 0) continue;
        const layout_node* n = b.find(nodes_[i].element_hash);
        if (n == nullptr) continue;
        // Along the PARENT split's axis — that is the axis this node's `sizing` governs.
        const node_index p = nodes_[i].parent;
        const bool horizontal = p == no_node || nodes_[p].dir == direction::HORIZONTAL;
        // Screen space: the workspace's rects are compared against raw cursor coordinates by the
        // dock drag, which knows nothing about surfaces.
        const bounding_box box = b.screen_box(*n);
        nodes_[i].resolved = horizontal ? box.dimension.width : box.dimension.height;
        nodes_[i].box = panel_rect{ static_cast<float>(box.x), static_cast<float>(box.y),
                                    static_cast<float>(box.dimension.width),
                                    static_cast<float>(box.dimension.height) };
    }
}

// --- Drag to dock --------------------------------------------------------------------------------

void Workspace::begin_card_drag(std::uint64_t card, std::uint64_t element) noexcept
{
    if (!contains(card)) return;
    drag_ = {};
    drag_.active = true;
    drag_.card = card;
    drag_.element = element;
}

void Workspace::update_card_drag(float cursor_x, float cursor_y) noexcept
{
    if (!drag_.active) return;
    drag_.target = no_node;
    drag_.as_tab = false;

    // Innermost panel under the cursor wins. Panels never overlap within the docked tree, and a
    // floating panel found later in the scan naturally takes precedence over a docked one beneath it.
    for (std::size_t i = 0; i < nodes_.size(); ++i)
    {
        if (dead_[i] || !nodes_[i].is_panel()) continue;
        const panel_rect& r = nodes_[i].box;
        if (r.width <= 0.0f || r.height <= 0.0f) continue;
        if (cursor_x < r.x || cursor_x >= r.right() || cursor_y < r.y || cursor_y >= r.bottom())
            continue;
        drag_.target = static_cast<node_index>(i);
    }
    if (drag_.target == no_node) return;

    // Zone: the outer quarter of each edge splits, the middle joins the tabs. Quarters rather than
    // halves so the tab zone stays comfortably hittable on a small panel.
    const panel_rect& r = nodes_[drag_.target].box;
    const float fx = (cursor_x - r.x) / r.width;
    const float fy = (cursor_y - r.y) / r.height;
    constexpr float kEdge = 0.25f;
    const float left_d = fx, right_d = 1.0f - fx, top_d = fy, bottom_d = 1.0f - fy;
    const float nearest = std::min({ left_d, right_d, top_d, bottom_d });
    if (nearest > kEdge)
    {
        drag_.as_tab = true;
        return;
    }
    if (nearest == left_d)        drag_.zone = side::Left;
    else if (nearest == right_d)  drag_.zone = side::Right;
    else if (nearest == top_d)    drag_.zone = side::Top;
    else                          drag_.zone = side::Bottom;
}

bool Workspace::end_card_drag(panel_rect fallback) noexcept
{
    if (!drag_.active) { drag_ = {}; return false; }
    const card_drag d = drag_;
    drag_ = {};

    if (d.target == no_node)
        return dock(d.card, floating_at(fallback));   // dropped over nothing: it floats

    // Dropping onto its own panel is a no-op rather than a rebuild — `dock` would reject a
    // self-referential move anyway, but bailing here avoids a pointless collapse/insert cycle.
    if (panel_of(d.card) == d.target && (d.as_tab || nodes_[d.target].cards.size() == 1))
        return false;

    const std::uint64_t anchor = nodes_[d.target].cards.empty() ? 0 : nodes_[d.target].cards.front();
    if (anchor == 0) return false;
    if (d.as_tab)
        return dock(d.card, tab_with(anchor));

    placement p;
    switch (d.zone)
    {
        case side::Left:   p = left_of(anchor); break;
        case side::Right:  p = right_of(anchor); break;
        case side::Top:    p = top_of(anchor); break;
        case side::Bottom: p = bottom_of(anchor); break;
    }
    return dock(d.card, p);
}

std::uint16_t Workspace::effective_min(node_index n, direction axis) const noexcept
{
    if (!valid(n)) return 0;
    if (nodes_[n].is_panel()) return min_region_;
    const int a = effective_min(nodes_[n].a, axis);
    const int b = effective_min(nodes_[n].b, axis);
    // Along the split's own axis the children sit END TO END, so their minimums add (plus the
    // splitter between them). Across it they sit SIDE BY SIDE, so the larger one governs.
    const int v = (nodes_[n].dir == axis) ? a + b + static_cast<int>(splitter_px_) : std::max(a, b);
    return static_cast<std::uint16_t>(std::min(v, 0xFFFF));
}

void Workspace::drag_splitter(node_index split, float delta, std::uint16_t origin_a,
                              std::uint16_t origin_b) noexcept
{
    if (!valid(split) || !nodes_[split].is_split() || nodes_[split].locked)
        return;

    // Both children keep their combined extent — a splitter moves the boundary, it does not resize
    // the parent. Clamped so neither side can be driven negative (or past the pair's total).
    const int total = static_cast<int>(origin_a) + static_cast<int>(origin_b);
    if (total <= 0) return;
    // The floor PERCENT does not give for free (GROW started at content size; percent ignores it).
    // Without this a region is draggable below its own chrome, the overflow shrink squeezes its
    // children toward zero, and the text garbles.
    // Per-CHILD minimums, not a flat one: a child that is itself a split needs room for its whole
    // subtree, and clamping both sides to the same panel-sized floor is what let a three-way split
    // be dragged until the inner pair overflowed.
    const direction axis = nodes_[split].dir;
    const int min_a = static_cast<int>(effective_min(nodes_[split].a, axis));
    const int min_b = static_cast<int>(effective_min(nodes_[split].b, axis));
    int a_px = static_cast<int>(std::lround(static_cast<float>(origin_a) + delta));
    a_px = std::clamp(a_px, min_a, std::max(min_a, total - min_b));
    const int b_px = total - a_px;

    workspace_node& ca = nodes_[nodes_[split].a];
    workspace_node& cb = nodes_[nodes_[split].b];

    // Write back in whatever currency each side already uses, so a drag never silently converts a
    // fixed strip into a proportional one (or vice versa) — the author's intent survives the gesture.
    if (ca.sizing.mode == size_mode::FIXED)
    {
        ca.sizing.value = static_cast<std::uint16_t>(a_px);
        if (cb.sizing.mode == size_mode::FIXED)
            cb.sizing.value = static_cast<std::uint16_t>(b_px);
    }
    else if (cb.sizing.mode == size_mode::FIXED)
    {
        cb.sizing.value = static_cast<std::uint16_t>(b_px);
    }
    else if (ca.sizing.mode == size_mode::PERCENT && cb.sizing.mode == size_mode::PERCENT)
    {
        // THE path a dock splitter takes. Percent ignores content, so pixels -> per-mille -> pixels
        // round-trips exactly and the splitter stays under the cursor instead of jumping.
        constexpr int kPerMille = 1000;
        const int pa = std::clamp(a_px * kPerMille / total, 1, kPerMille - 1);
        ca.sizing.value = static_cast<std::uint16_t>(pa);
        cb.sizing.value = static_cast<std::uint16_t>(kPerMille - pa);
    }
    else
    {
        // Both GROW: rescale the weights, renormalising to a FIXED total so the sum stays stable
        // across repeated drags AND there is resolution to express the new ratio.
        //
        // Preserving the children's EXISTING sum instead does not work: default weights are 1 and 1,
        // so the sum is 2 and the clamp range collapses to [1, 1] — the weights can never change.
        //
        // NOTE this branch cannot make a splitter track the cursor exactly, because GROW starts at
        // CONTENT size and only shares the surplus: converting resolved pixels to a weight and back
        // does not round-trip. Splits therefore use PERCENT (see split_node); this is kept for the
        // general weighted-grow case, which is not a splitter.
        constexpr int kWeightTotal = 1000;
        const int wa = std::clamp(a_px * kWeightTotal / total, 1, kWeightTotal - 1);
        ca.sizing.weight = static_cast<std::uint16_t>(wa);
        cb.sizing.weight = static_cast<std::uint16_t>(kWeightTotal - wa);
    }
}

void Workspace::free_node(node_index n) noexcept
{
    if (n == no_node) return;
    dead_[n] = 1;
    nodes_[n].cards.clear();
    nodes_[n].parent = no_node;
    nodes_[n].a = no_node;
    nodes_[n].b = no_node;
    free_.push_back(n);
}

std::size_t Workspace::node_count() const noexcept
{
    return static_cast<std::size_t>(std::count(dead_.begin(), dead_.end(), std::uint8_t{ 0 }));
}

void Workspace::clear() noexcept
{
    nodes_.clear();
    dead_.clear();
    free_.clear();
    floating_.clear();
    root_ = no_node;
}

// --- Queries -------------------------------------------------------------------------------------

node_index Workspace::panel_of(std::uint64_t card) const noexcept
{
    if (card == 0) return no_node;
    for (std::size_t i = 0; i < nodes_.size(); ++i)
    {
        if (dead_[i] || !nodes_[i].is_panel()) continue;
        const auto& c = nodes_[i].cards;
        if (std::find(c.begin(), c.end(), card) != c.end())
            return static_cast<node_index>(i);
    }
    return no_node;
}

bool Workspace::contains(std::uint64_t card) const noexcept
{
    return panel_of(card) != no_node;
}

// --- Structure -----------------------------------------------------------------------------------

void Workspace::replace_in_parent(node_index old, node_index child) noexcept
{
    const node_index parent = nodes_[old].parent;
    if (parent != no_node)
    {
        if (nodes_[parent].a == old) nodes_[parent].a = child;
        else if (nodes_[parent].b == old) nodes_[parent].b = child;
        if (child != no_node) nodes_[child].parent = parent;
        return;
    }
    // A root: either the docked root or one of the floating roots.
    if (root_ == old)
    {
        root_ = child;
        if (child != no_node) nodes_[child].parent = no_node;
        return;
    }
    const auto it = std::find(floating_.begin(), floating_.end(), old);
    if (it == floating_.end())
        return;
    if (child == no_node)
    {
        floating_.erase(it);
        return;
    }
    *it = child;
    nodes_[child].parent = no_node;
}

void Workspace::collapse_if_empty(node_index panel) noexcept
{
    if (panel == no_node || !nodes_[panel].is_panel() || !nodes_[panel].cards.empty())
        return;

    const node_index parent = nodes_[panel].parent;
    if (parent == no_node)
    {
        // A root with nothing in it: drop it. (A floating root that empties is deleted outright.)
        replace_in_parent(panel, no_node);
        free_node(panel);
        return;
    }

    // The surviving sibling takes the split's place AND ITS SIZING. Inheriting the sizing is what
    // makes "the ratios merge" precise: the split occupied a region of a given size within ITS
    // parent, and the survivor now occupies exactly that region.
    const node_index sibling = (nodes_[parent].a == panel) ? nodes_[parent].b : nodes_[parent].a;
    nodes_[sibling].sizing = nodes_[parent].sizing;
    replace_in_parent(parent, sibling);
    free_node(parent);
    free_node(panel);
}

void Workspace::detach(std::uint64_t card) noexcept
{
    const node_index p = panel_of(card);
    if (p == no_node) return;
    auto& cards = nodes_[p].cards;
    const auto it = std::find(cards.begin(), cards.end(), card);
    const auto pos = static_cast<std::size_t>(it - cards.begin());
    cards.erase(it);
    if (nodes_[p].selected >= cards.size() && !cards.empty())
        nodes_[p].selected = cards.size() - 1;
    else if (pos < nodes_[p].selected)
        --nodes_[p].selected;   // keep the same tab visible when one before it goes away
    collapse_if_empty(p);
}

node_index Workspace::new_panel(std::uint64_t card, axis_sizing s)
{
    workspace_node n;
    n.type = workspace_node::kind::Panel;
    n.sizing = s;
    n.cards.push_back(card);
    n.selected = 0;
    return alloc(std::move(n));
}

bool Workspace::split_node(node_index target, std::uint64_t card, const placement& where)
{
    // A non-definite split becomes a PERCENT pair rather than GROW siblings. GROW cannot express a
    // resizable split: a GROW child starts at its content size and only shares the surplus, so a
    // splitter that converts resolved pixels into weights does not round-trip and jumps on the first
    // click. PERCENT ignores content, so pixels <-> proportion is exact. A FIXED placement is left
    // alone: a fixed strip beside a growing remainder already round-trips in pixels.
    axis_sizing fresh_sizing = where.sizing;
    axis_sizing rest_sizing = grow();
    if (where.sizing.mode != size_mode::FIXED)
    {
        const std::uint16_t pm =
            where.sizing.mode == size_mode::PERCENT ? where.sizing.value : std::uint16_t{ 500 };
        // The min goes into the SIZING too, not just the drag clamp: a window resize can squeeze a
        // region below the floor without any drag being involved.
        const std::uint16_t floor_px = std::max(where.sizing.min, min_region_);
        fresh_sizing = percent(pm, floor_px, where.sizing.max);
        rest_sizing = percent(static_cast<std::uint16_t>(1000 - pm), floor_px);
    }

    const node_index fresh = new_panel(card, fresh_sizing);

    workspace_node s;
    s.type = workspace_node::kind::Split;
    s.dir = axis_of(where.where);
    s.locked = where.lock;
    // The split takes the region the target occupied, so the arrangement around it is undisturbed.
    s.sizing = nodes_[target].sizing;
    const node_index split = alloc(std::move(s));

    // `target` keeps its position in the tree but now sizes as the remainder: the new region carries
    // the explicit size, the old one takes what is left.
    replace_in_parent(target, split);
    nodes_[target].sizing = rest_sizing;
    nodes_[target].parent = split;
    nodes_[fresh].parent = split;
    nodes_[split].a = leading(where.where) ? fresh : target;
    nodes_[split].b = leading(where.where) ? target : fresh;
    return true;
}

// --- Verbs ---------------------------------------------------------------------------------------

bool Workspace::dock(std::uint64_t card, const placement& where)
{
    if (card == 0) return false;

    // Resolve the anchor BEFORE detaching: detaching can collapse the very panel we are targeting,
    // and resolving first means a self-referential move (docking a card beside itself) fails cleanly
    // instead of dangling.
    node_index target = no_node;
    if (where.mode == placement::kind::Tab || where.anchor != 0)
    {
        target = panel_of(where.anchor);
        if (target == no_node) return false;
        if (nodes_[target].cards.size() == 1 && nodes_[target].cards[0] == card)
            return false;   // moving a card relative to itself is a no-op, not a structure change
    }

    detach(card);

    // Detaching may have collapsed the target away (it held only this card).
    if (target != no_node && (dead_[target] || !nodes_[target].is_panel()))
        return false;

    switch (where.mode)
    {
        case placement::kind::Tab:
            nodes_[target].cards.push_back(card);
            nodes_[target].selected = nodes_[target].cards.size() - 1;
            return true;

        case placement::kind::Floating:
        {
            const node_index p = new_panel(card, grow());
            // The rect is the whole point of a floating placement. Dropping it here left every torn
            // -off panel at 0x0 — which then had nothing to hit-test, so it could not be moved either.
            nodes_[p].floating_state.rect = where.rect;
            nodes_[p].floating_state.placed = true;
            floating_.push_back(p);   // newest on top
            return true;
        }

        case placement::kind::Side:
            if (target == no_node)
            {
                // Root-relative. An empty workspace just gets its first panel.
                if (root_ == no_node)
                {
                    root_ = new_panel(card, grow());
                    return true;
                }
                target = root_;
            }
            return split_node(target, card, where);
    }
    return false;
}

bool Workspace::undock(std::uint64_t card, panel_rect rect)
{
    if (!contains(card)) return false;
    placement p = floating_at(rect);
    return dock(card, p);
}

bool Workspace::close(std::uint64_t card)
{
    if (!contains(card)) return false;
    detach(card);
    return true;
}

bool Workspace::select(std::uint64_t card)
{
    const node_index p = panel_of(card);
    if (p == no_node) return false;
    const auto& cards = nodes_[p].cards;
    nodes_[p].selected =
        static_cast<std::size_t>(std::find(cards.begin(), cards.end(), card) - cards.begin());
    return true;
}

void Workspace::bring_to_front(node_index floating_root) noexcept
{
    const auto it = std::find(floating_.begin(), floating_.end(), floating_root);
    if (it == floating_.end() || it + 1 == floating_.end())
        return;
    std::rotate(it, it + 1, floating_.end());
}

}  // namespace string::ui
