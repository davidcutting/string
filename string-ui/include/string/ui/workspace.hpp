#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <string/ui/layout.hpp>
#include <string/ui/panel.hpp>

// The workspace (brief 12 M2b, L5) — the arrangement of dockable cards.
//
// THE INVARIANT TO DEFEND IN REVIEW: this file describes STRUCTURE and computes NO GEOMETRY. There
// is deliberately no function here that returns a rect. The workspace walks and EMITS into the
// layout builder (M2c) — a split becomes a row/column container, a panel becomes a tab bar plus the
// selected card — so every rect still comes from the one layout engine. A dock tree that computes
// rects re-implements layout in miniature; one that expands into layout does not.
//
// The only thing that makes this different from the layout tree is WHO AUTHORS IT: the layout tree
// is written by the programmer and rebuilt every frame, while this is edited by the user at runtime
// and must survive that rebuild.
namespace string::ui
{

using node_index = std::uint32_t;
inline constexpr node_index no_node = 0xFFFFFFFFu;

// Which side of the anchor the new card lands on. Left/Right imply a HORIZONTAL split, Top/Bottom a
// VERTICAL one.
enum class side : std::uint8_t
{
    Left,
    Right,
    Top,
    Bottom,
};

// Where a card goes. A VALUE, built by the free factories below — `dock`/`undock`/`close` are the
// only verbs, and an earlier `dock_target` type was dropped for reintroducing "dock" as a noun.
struct placement
{
    enum class kind : std::uint8_t
    {
        Side,       // split `anchor`'s panel (or the root when anchor == 0)
        Tab,        // join `anchor`'s panel as another tab
        Floating,   // a new floating panel at `rect`
    };

    kind mode = kind::Side;
    side where = side::Left;
    std::uint64_t anchor = 0;          // card hash; 0 = relative to the whole docked root
    axis_sizing sizing = grow();       // the NEW region's size along the split axis
    bool lock = false;                 // is the resulting splitter draggable?
    panel_rect rect{};                 // Floating only

    // Marks the resulting splitter undraggable. Independent of `sizing`: a locked proportional
    // region still resizes with the window, and an unlocked fixed region can be dragged to a new
    // pixel size without scaling. Docking INTO a locked region still works — only the edge is fixed.
    [[nodiscard]] placement locked() const
    {
        placement p = *this;
        p.lock = true;
        return p;
    }
};

// --- Placement factories (free functions, matching grow()/fit()/fixed()) -------------------------
// Root-relative: split the whole docked arrangement.
[[nodiscard]] placement left(axis_sizing s = grow()) noexcept;
[[nodiscard]] placement right(axis_sizing s = grow()) noexcept;
[[nodiscard]] placement top(axis_sizing s = grow()) noexcept;
[[nodiscard]] placement bottom(axis_sizing s = grow()) noexcept;

// Card-relative: split the panel CONTAINING that card. A card is never a region on its own — only
// the panel holding it is — which is why these are unambiguous.
[[nodiscard]] placement left_of(std::uint64_t card, axis_sizing s = grow()) noexcept;
[[nodiscard]] placement right_of(std::uint64_t card, axis_sizing s = grow()) noexcept;
[[nodiscard]] placement top_of(std::uint64_t card, axis_sizing s = grow()) noexcept;
[[nodiscard]] placement bottom_of(std::uint64_t card, axis_sizing s = grow()) noexcept;

// Join an existing panel as another tab.
[[nodiscard]] placement tab_with(std::uint64_t card) noexcept;

// A new floating panel.
[[nodiscard]] placement floating_at(panel_rect r) noexcept;

// --- Nodes ---------------------------------------------------------------------------------------

// Two node kinds, and no more. A LEAF IS A PANEL: a single-card panel is a panel with one tab, so
// there is no separate leaf type and therefore no leaf<->tabs transitions to get wrong. That is
// most of why the collapse rules below stay small.
struct workspace_node
{
    enum class kind : std::uint8_t
    {
        Split,
        Panel,
    };

    kind type = kind::Panel;

    // Stable across frames and across slot recycling, unlike the node index. Element ids and
    // persisted state key off this, so a freed slot handed back out cannot inherit the old node's
    // drag state or hover.
    std::uint64_t uid = 0;

    // --- Transient, set by observe() after layout; NOT serialised -------------------------------
    // The element id this node emitted as, and its resolved size along the parent split's axis.
    // Splitter drag needs the latter to convert a pixel delta into a sizing change, and sizes only
    // exist after layout — see Workspace::observe.
    std::uint64_t element_hash = 0;
    std::uint16_t resolved = 0;
    // The full box layout gave this node. CACHED, not computed — `observe()` copies what the layout
    // engine produced. Drop-zone hit-testing needs to know where the panels ARE, and that only exists
    // after layout. This does not breach "the workspace computes no geometry": it consumes a rect,
    // it never derives one.
    panel_rect box{};
    // Splitter drag origin, captured on the frame the drag starts (see M1's `update_panel`: the
    // delta is absolute-from-press, so recomputing from a captured origin is what keeps clamping
    // idempotent instead of drifting).
    bool dragging = false;
    std::uint16_t drag_a = 0;
    std::uint16_t drag_b = 0;

    // How this node sizes itself along its PARENT split's axis (cross axis always grows). Sizing
    // lives on NODES, not on splits: a fixed 280px strip is a property of one child, not of the
    // relationship — the same reason the layout engine puts sizing on children rather than ratios on
    // containers. Keeping the vocabulary identical to layout's makes emission a direct translation.
    axis_sizing sizing = grow();
    node_index parent = no_node;

    // --- Split ---
    direction dir = direction::HORIZONTAL;
    bool locked = false;   // is this splitter draggable?
    node_index a = no_node;
    node_index b = no_node;

    // --- Panel ---
    std::vector<std::uint64_t> cards;   // ordered = tab order
    std::size_t selected = 0;
    // Position and drag bookkeeping, used only while this panel is a FLOATING root. Reuses M1's
    // `panel_state` so floating workspace panels move and resize through exactly the same
    // `update_panel` the standalone panels do — floating is not a second mechanism.
    panel_state floating_state{};

    [[nodiscard]] bool is_split() const noexcept { return type == kind::Split; }
    [[nodiscard]] bool is_panel() const noexcept { return type == kind::Panel; }
};

// --- Workspace -----------------------------------------------------------------------------------

// A FOREST: one docked tree filling the container it is emitted into, plus N floating panels.
//
// Floating and docked are NOT two mechanisms — a floating panel is the same object as a docked one,
// just rooted on its own. So every gesture is a SUBTREE MOVE (tear-off, drag-to-dock and undock are
// the same operation), and there is no floating<->docked conversion path to write.
class Workspace
{
public:
    // --- Verbs -----------------------------------------------------------------------------------
    // Places `card` per `where`. If the card is already in the workspace this MOVES it, so the
    // gestures and the seed use one code path. Returns false if the placement cannot be resolved
    // (an anchor that is not present).
    bool dock(std::uint64_t card, const placement& where);

    // Moves `card` out to its own floating panel. Same operation as a tear-off gesture.
    bool undock(std::uint64_t card, panel_rect rect = { 80.0f, 80.0f, 320.0f, 240.0f });

    // Removes `card` entirely.
    bool close(std::uint64_t card);

    // Makes `card` the visible tab of its panel.
    bool select(std::uint64_t card);

    // Raises a floating panel to the front of the floating order.
    void bring_to_front(node_index floating_root) noexcept;

    // --- Queries ---------------------------------------------------------------------------------
    [[nodiscard]] bool contains(std::uint64_t card) const noexcept;
    [[nodiscard]] node_index panel_of(std::uint64_t card) const noexcept;
    [[nodiscard]] node_index root() const noexcept { return root_; }
    [[nodiscard]] bool empty() const noexcept { return root_ == no_node && floating_.empty(); }

    // Back-to-front: floating().back() is the frontmost panel.
    [[nodiscard]] std::span<const node_index> floating() const noexcept { return floating_; }

    [[nodiscard]] const workspace_node& node(node_index n) const { return nodes_[n]; }
    [[nodiscard]] workspace_node& node(node_index n) { return nodes_[n]; }
    [[nodiscard]] bool valid(node_index n) const noexcept
    {
        return n != no_node && n < nodes_.size() && !dead_[n];
    }

    // Live node count (excludes recycled slots) — for tests and the debug inspector.
    [[nodiscard]] std::size_t node_count() const noexcept;

    // POST-LAYOUT. Caches each emitted node's resolved size along its parent split's axis.
    //
    // The one place the workspace reads layout back, and deliberately narrow: a splitter drag turns
    // a pixel delta into a sizing change, and no author-time information can do that — the
    // container's size does not exist until layout has run. A CONVERSION FACTOR, read once per frame
    // and used only while a splitter is held; never fed back into the serialised arrangement.
    //
    // This does NOT violate "the workspace computes no geometry": it consumes a size the layout
    // engine produced rather than deriving one.
    void observe(const layout_builder& b) noexcept;

    // Applies a splitter drag: `delta` is the cursor movement along the split's axis, in pixels,
    // measured from where the drag began. `origin_a`/`origin_b` are the two children's resolved
    // sizes at press time. Pure — the caller owns capture-at-press.
    void drag_splitter(node_index split, float delta, std::uint16_t origin_a,
                       std::uint16_t origin_b) noexcept;

    void clear() noexcept;

    // --- Drag-to-dock (M2d) ------------------------------------------------------------------------
    //
    // Tear-off, drag-to-dock and reorder are ONE gesture: pick a card up, and on release it is
    // `dock`ed wherever the cursor is — into a panel as a tab, beside one as a split, or nowhere,
    // which means floating. There is no separate tear-off path because there is no
    // floating<->docked conversion (see the forest note above).
    struct card_drag
    {
        bool active = false;
        std::uint64_t card = 0;           // the card being carried
        std::uint64_t element = 0;        // the element id whose pointer capture drives it
        node_index target = no_node;      // panel under the cursor, or no_node for "drop as floating"
        side zone = side::Left;           // which edge of the target
        bool as_tab = false;              // true = join the target's tabs rather than split it
    };

    void begin_card_drag(std::uint64_t card, std::uint64_t element) noexcept;
    // Resolves target + zone from the boxes `observe()` cached. Call each frame while dragging.
    void update_card_drag(float cursor_x, float cursor_y) noexcept;
    // Commits: docks the carried card at the resolved target, or floats it at `fallback` when the
    // cursor is over nothing. Returns true if anything moved.
    bool end_card_drag(panel_rect fallback) noexcept;
    void cancel_card_drag() noexcept { drag_ = {}; }
    [[nodiscard]] const card_drag& dragging_card() const noexcept { return drag_; }

    // Smallest a region may be dragged to, in pixels along the split axis.
    //
    // PERCENT deliberately ignores content — that is what makes a splitter round-trip — so unlike
    // GROW it does NOT give a content-derived floor for free. Without this, a region can be driven
    // below what its own chrome needs; the layout's overflow shrink then squeezes its children
    // toward zero and the text garbles. Author-set rather than measured, for the same reason panel
    // minimums are (see brief 12, "Overflow policy"): deriving it from content would need a
    // post-layout writeback and is interim work until scrolling exists.
    //
    // The default clears a tab bar plus one row of body text.
    void set_min_region(std::uint16_t px) noexcept { min_region_ = px; }
    [[nodiscard]] std::uint16_t min_region() const noexcept { return min_region_; }

    // Visual thickness of a splitter, in pixels. Only used to compute the minimum below; the emit
    // layer owns the actual look.
    void set_splitter_px(std::uint16_t px) noexcept { splitter_px_ = px; }
    [[nodiscard]] std::uint16_t splitter_px() const noexcept { return splitter_px_; }

    // The smallest this node can be along `axis` without its own contents overflowing.
    //
    // A SPLIT's minimum is NOT `min_region`: it is what its whole subtree needs. Along the split's
    // own axis that is the sum of both children's minimums plus the splitter; ACROSS it, the larger
    // of the two, since the children sit side by side rather than end to end.
    //
    // Getting this wrong is what let a three-way split break: the outer splitter could be dragged
    // until the inner pair no longer fitted, at which point they overflowed their container and
    // visually overlapped the neighbouring region. A flat per-panel minimum cannot see that, because
    // the constraint belongs to the subtree, not the panel.
    [[nodiscard]] std::uint16_t effective_min(node_index n, direction axis) const noexcept;

private:
    node_index alloc(workspace_node n);
    void free_node(node_index n) noexcept;

    // Puts `child` where `old` sat — in `old`'s parent split, as the docked root, or in the floating
    // list — and fixes up parent links. The single place structural replacement happens.
    void replace_in_parent(node_index old, node_index child) noexcept;

    // THE COLLAPSE. Called after a card leaves a panel: if the panel is now empty, remove it; if it
    // had a parent split, replace that split with its surviving sibling, WHICH INHERITS THE SPLIT'S
    // SIZING. Three steps, no type transitions — the whole reason a leaf is a panel.
    void collapse_if_empty(node_index panel) noexcept;

    // Detaches `card` from whatever panel holds it and collapses. Used by dock() to make a re-dock a
    // move rather than a duplicate.
    void detach(std::uint64_t card) noexcept;

    node_index new_panel(std::uint64_t card, axis_sizing s);
    bool split_node(node_index target, std::uint64_t card, const placement& where);

    std::vector<workspace_node> nodes_;
    std::vector<std::uint8_t> dead_;      // parallel to nodes_; 1 = recycled slot
    std::vector<node_index> free_;
    node_index root_ = no_node;
    std::vector<node_index> floating_;    // back-to-front
    std::uint64_t next_uid_ = 1;
    std::uint16_t min_region_ = 96;
    std::uint16_t splitter_px_ = 6;
    card_drag drag_{};
};

}  // namespace string::ui
