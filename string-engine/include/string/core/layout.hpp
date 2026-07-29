#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace string
{

struct dimension
{
    uint16_t width;
    uint16_t height;
};

struct bounding_box
{
    uint16_t x;
    uint16_t y;
    dimension dimension;

    [[nodiscard]] constexpr bool contains(uint16_t px, uint16_t py) const noexcept
    {
        return px >= x && px < x + dimension.width
            && py >= y && py < y + dimension.height;
    }
};

struct padding
{
    uint16_t left = 0;
    uint16_t right = 0;
    uint16_t top = 0;
    uint16_t bottom = 0;
};

struct color
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

// FNV-1a, so an id can carry a cheap integer key for lookup / hit-testing / diffing.
[[nodiscard]] constexpr uint64_t fnv1a(std::string_view s) noexcept
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char c : s)
    {
        h ^= static_cast<uint8_t>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}

// `name` is a non-owning view — the string it refers to must outlive any renderable produced
// from it (string literals, incl. compile-time, are fine). Prefer the "..."_id literal or
// make_id() so `hash` is populated; a bare aggregate leaves hash == 0.
struct id
{
    std::string_view name;
    uint64_t hash = 0;
};

[[nodiscard]] constexpr id make_id(std::string_view name) noexcept
{
    return id{ name, fnv1a(name) };
}

namespace literals
{
[[nodiscard]] consteval id operator""_id(const char* s, std::size_t n)
{
    const std::string_view view{ s, n };
    return id{ view, fnv1a(view) };
}
}  // namespace literals

// Cross-axis placement of children within a container.
enum class alignment : uint8_t
{
    TOP,
    LEFT,
    CENTER,
    RIGHT,
    BOTTOM,
};

// Main-axis distribution of a container's leftover space among its children.
enum class justification : uint8_t
{
    START,
    CENTER,
    END,
    SPACE_BETWEEN,   // first at start, last at end, equal space between
    SPACE_AROUND,    // equal space around each child (half at the ends)
    SPACE_EVENLY,    // equal space between and at the ends
};

enum class direction : uint8_t
{
    VERTICAL,
    HORIZONTAL,
};

enum class shape : uint8_t
{
    RECTANGLE,          // default: sharp corners (radius ignored)
    ROUNDED_RECTANGLE,  // corners rounded by `radius`
    CIRCLE,             // filled circle/pill: corner radius = half the smaller side
};

// How an element resolves its size on a single axis.
enum class size_mode : uint8_t
{
    FIT,    // shrink-wrap: content (measured, or children) size, floored by `value`
    FIXED,  // exactly `value` — never grows or shrinks
    GROW,   // start at content size, then expand to fill leftover (and shrink on overflow)
};

struct axis_sizing
{
    size_mode mode = size_mode::FIT;
    uint16_t value = 0;         // the size when FIXED (and the floor for a FIT leaf)
    uint16_t min = 0;           // hard floor (also the floor when shrinking on overflow)
    uint16_t max = 0xFFFF;
};

struct sizing
{
    axis_sizing width;
    axis_sizing height;
};

// Ergonomic sizing factories.
[[nodiscard]] constexpr axis_sizing fixed(uint16_t value) noexcept { return { size_mode::FIXED, value }; }
[[nodiscard]] constexpr axis_sizing fit(uint16_t floor = 0) noexcept { return { size_mode::FIT, floor }; }
[[nodiscard]] constexpr axis_sizing grow(uint16_t min = 0, uint16_t max = 0xFFFF) noexcept
{
    return { size_mode::GROW, 0, min, max };
}
[[nodiscard]] constexpr sizing size_fixed(uint16_t w, uint16_t h) noexcept { return { fixed(w), fixed(h) }; }
[[nodiscard]] constexpr sizing size_grow() noexcept { return { grow(), grow() }; }
[[nodiscard]] constexpr sizing size_fit() noexcept { return { fit(), fit() }; }

struct element
{
    id id;
    color color;            // fill
    // Elaborated 'struct color': the member 'color' above hides the type name in this scope.
    struct color stroke_color;   // border colour (used when stroke_width > 0)
    uint16_t radius;        // corner radius (px), honored for ROUNDED_RECTANGLE
    uint16_t stroke_width;  // border width (px); 0 = no border
    shape shape;            // RECTANGLE by default
    sizing sizing;          // how this element sizes itself on each axis
    // 1-based index into the layout_builder's text table (0 = no text). Text content is held
    // out-of-line (see text_run) so `element` stays small and trivially copyable, and so text
    // falls out as its own buffer for a text pass — the string itself never rides in the element.
    // A text measurer resolves this index against the table (see core/text_measurer.hpp).
    uint32_t text = 0;
    // Absolute-positioned ("floating") overlay escape. A floating node (and its subtree) is placed
    // at (float_x, float_y) in the ROOT's coordinate space, ignoring its parent's flow — and it does
    // not contribute to its parent's fit size or push siblings. This is the overlay/anchor primitive
    // world-anchored UI (nameplates, ping markers, tooltips, radial menus) needs; without it every
    // node is packed into the flow. Default off: existing layouts are unchanged.
    bool floating = false;
    uint16_t float_x = 0;
    uint16_t float_y = 0;
    // Topmost render layer (inherited by the subtree): the renderer draws overlay content — shapes
    // AND text — after ALL non-overlay content, so a modal surface (debug console, HUD) covers the
    // scene UI's text as well as its shapes (shapes and glyphs are separate draw streams, so tree
    // order alone cannot put a later shape over an earlier node's text). Layout ignores it.
    bool overlay = false;
    // Radial cooldown-sweep fraction (0 = none/ready, 255 = fully on cooldown). A renderer that
    // supports it (the UI overlay's shape shader) dims the not-yet-elapsed clockwise wedge — the
    // ability/cooldown affordance games use. Purely visual; layout ignores it.
    uint8_t sweep = 0;
};

// A run of text attached to an element, held in the layout_builder's side-table. `str` is
// non-owning — it must outlive any layout/renderable produced from it, the same contract as
// id.name. `font_px` is the requested pixel size (0 = the renderer's default).
struct text_run
{
    std::string_view str;
    uint16_t font_px = 0;
};

struct format
{
    padding padding = {};
    uint16_t gap = 0;
    alignment alignment = alignment::TOP;    // cross-axis alignment of children
    direction direction = direction::VERTICAL;  // main axis along which children are stacked
    justification justify = justification::START;  // main-axis distribution of leftover space
};

// The compact "flattened" output type (element + computed box), used for the baked array path.
struct renderable_element
{
    element element;
    bounding_box bounding_box;
};

// A measurer returns the intrinsic content size of a leaf element (e.g. a text run). Anything
// invocable as dimension(const element&) qualifies.
template <typename M>
concept measurer = std::invocable<M&, const element&>;

// The default: no intrinsic content (leaves fall back to their FIXED value / FIT floor).
struct no_measure
{
    constexpr dimension operator()(const element&) const noexcept { return { 0, 0 }; }
};

// Intrusive tree links (first-child / next-sibling) as 32-bit indices into a contiguous
// pool. A derived node inherits these and adds only its payload — no per-node allocation,
// the whole tree lives in one cache-friendly array. `deducing this` recovers the concrete
// node type during traversal, so there is no template parameter to specify.
struct tree_node
{
    static constexpr uint32_t none = 0xFFFFFFFFu;

    uint32_t parent = none;
    uint32_t first_child = none;
    uint32_t last_child = none;
    uint32_t next_sibling = none;

    [[nodiscard]] constexpr bool is_root() const noexcept { return parent == none; }
    [[nodiscard]] constexpr bool is_leaf() const noexcept { return first_child == none; }

    // Append `child` as the last child of `parent` within `pool` (O(1)).
    template <typename Pool>
    static constexpr void link(Pool& pool, uint32_t parent, uint32_t child) noexcept
    {
        auto& p = pool[parent];
        pool[child].parent = parent;
        pool[child].next_sibling = none;
        if (p.last_child == none)
            p.first_child = child;
        else
            pool[p.last_child].next_sibling = child;
        p.last_child = child;
    }

    // Invoke fn(child, index) for each child in insertion order. The concrete node type is
    // deduced from the object via the explicit `this Self&` parameter.
    template <typename Self, typename Pool, typename Fn>
    constexpr void for_each_child(this const Self& self, Pool& pool, Fn&& fn)
    {
        for (uint32_t c = self.first_child; c != none; c = pool[c].next_sibling)
            fn(pool[c], c);
    }
};

// A layout tree node: its payload plus the intrusive links. After layout, the pool is in
// pre-order — which is exactly painter order — and each node carries its computed `box`, so
// consumers iterate nodes() directly (no separate flatten/copy). A node is a container iff
// !is_leaf().
struct layout_node : tree_node
{
    element element;
    format format;
    bounding_box box;
};

namespace detail
{

constexpr uint16_t to_u16(int value) noexcept
{
    return static_cast<uint16_t>(std::clamp(value, 0, 0xFFFF));
}

constexpr int main_axis_padding(const format& f) noexcept
{
    return f.direction == direction::HORIZONTAL
        ? f.padding.left + f.padding.right
        : f.padding.top + f.padding.bottom;
}

constexpr int cross_axis_padding(const format& f) noexcept
{
    return f.direction == direction::HORIZONTAL
        ? f.padding.top + f.padding.bottom
        : f.padding.left + f.padding.right;
}

// The resolved size of an axis given the content it must hold. GROW starts at content size
// and is flexed later, in flex_sizing().
constexpr int resolve_axis(const axis_sizing& s, int content) noexcept
{
    int base = 0;
    switch (s.mode)
    {
        case size_mode::FIXED: base = s.value; break;
        case size_mode::FIT:   base = std::max<int>(content, s.value); break;
        case size_mode::GROW:  base = content; break;
    }
    return std::clamp(base, static_cast<int>(s.min), static_cast<int>(s.max));
}

}  // namespace detail

class layout_scope;

// Immediate-mode layout builder. Everything is constexpr, so a layout can be computed at
// compile time (inside a constant-evaluated context) and, unchanged, at runtime.
//
//   builder.begin(panel, {.gap = 8, .direction = direction::VERTICAL})
//              .add_element(button_a)
//              .add_element(button_b)
//          .end();                          // fit-to-content
//   for (const auto& n : builder.nodes()) draw(n.element, n.box);
//
// end() variants control the root: end(extent) fills a viewport; end(measure) sizes leaves
// from a content-measurement hook; end(extent, measure) does both. container() returns an
// RAII scope that closes itself. clear() reuses the builder.
class layout_builder
{
    std::vector<layout_node> nodes_;
    std::vector<uint32_t> open_;
    // Text side-table; index 0 is a reserved "no text" sentinel, so element.text is 1-based.
    std::vector<text_run> texts_{ text_run{} };

public:
    // Open a container. `element` is its own visual/sizing; `format` lays out its children.
    constexpr auto begin(const element& element, const format& format) -> layout_builder&;
    // Open an invisible container that shrink-wraps its children.
    constexpr auto begin(const format& format) -> layout_builder&;
    // Add a leaf element to the current container.
    constexpr auto add_element(const element& element) -> layout_builder&;
    // Add a leaf text element: stores the run in the text table and points `element.text` at it, so
    // a text measurer sizes it and a text pass draws it. `element`'s own visuals (fill = text
    // colour, sizing) still apply; use size_fit() to shrink-wrap to the measured run.
    constexpr auto add_text(element element, std::string_view str, uint16_t font_px = 0) -> layout_builder&;

    // Close the current container. When it is the outermost one, its subtree is laid out.
    constexpr auto end() -> layout_builder&;
    // ...with the root filling `available`.
    constexpr auto end(const dimension& available) -> layout_builder&;
    // ...sizing leaves from a content measurer.
    template <measurer Measure>
    constexpr auto end(Measure measure) -> layout_builder&;
    // ...filling `available` and measuring leaves.
    template <measurer Measure>
    constexpr auto end(const dimension& available, Measure measure) -> layout_builder&;

    // RAII container: closes itself on scope exit (fit-to-content, no measurer).
    [[nodiscard]] constexpr auto container(const element& element, const format& format) -> layout_scope;
    [[nodiscard]] constexpr auto container(const format& format) -> layout_scope;

    // The positioned node pool, in painter order. Zero-copy; valid until clear()/rebuild.
    [[nodiscard]] constexpr auto nodes() const noexcept -> std::span<const layout_node>
    {
        return std::span<const layout_node>{ nodes_ };
    }

    // The text table, indexed by element.text (index 0 is the reserved "no text" sentinel).
    [[nodiscard]] constexpr auto text_runs() const noexcept -> std::span<const text_run>
    {
        return std::span<const text_run>{ texts_ };
    }

    // Snapshot the positioned nodes into a fixed-size array, so the result can escape constant
    // evaluation (live in a constexpr/static object). See evaluate_layout() for auto extent.
    template <std::size_t N>
    [[nodiscard]] constexpr auto to_array() const -> std::array<renderable_element, N>
    {
        std::array<renderable_element, N> out{};
        const std::size_t count = std::min(N, nodes_.size());
        for (std::size_t i = 0; i < count; ++i)
            out[i] = { nodes_[i].element, nodes_[i].box };
        return out;
    }

    // Find a positioned node by id hash (linear scan). nullptr if absent.
    [[nodiscard]] constexpr auto find(uint64_t id_hash) const noexcept -> const layout_node*
    {
        for (const layout_node& n : nodes_)
            if (n.element.id.hash == id_hash)
                return &n;
        return nullptr;
    }
    [[nodiscard]] constexpr auto find(const id& identifier) const noexcept -> const layout_node*
    {
        return find(identifier.hash);
    }

    // The top-most (last-drawn) node whose box contains (x, y). nullptr if none.
    [[nodiscard]] constexpr auto hit_test(uint16_t x, uint16_t y) const noexcept -> const layout_node*
    {
        const layout_node* hit = nullptr;
        for (const layout_node& n : nodes_)
            if (n.box.contains(x, y))
                hit = &n;   // later nodes are painted on top → keep the last match
        // An id-less top node (e.g. a button's label painted over it) is not interactive itself —
        // resolve to the nearest ancestor carrying an id so hover/click land on the widget, not
        // its text. A hit with no id'd ancestor returns as-is (empty-space click semantics).
        for (const layout_node* n = hit; n != nullptr;)
        {
            if (n->element.id.hash != 0)
                return n;
            n = n->is_root() ? nullptr : &nodes_[n->parent];
        }
        return hit;
    }

    constexpr void reserve(std::size_t node_count) { nodes_.reserve(node_count); }

    constexpr void clear() noexcept
    {
        nodes_.clear();
        open_.clear();
        texts_.clear();
        texts_.emplace_back();  // restore the index-0 "no text" sentinel
    }

private:
    template <measurer Measure>
    constexpr void close(bool has_available, dimension available, Measure& measure);
    template <measurer Measure>
    constexpr void fit_sizing(uint32_t index, Measure& measure);
    constexpr void flex_sizing(uint32_t index) noexcept;
    constexpr void position(uint32_t index, uint16_t origin_x, uint16_t origin_y) noexcept;
};

constexpr auto layout_builder::begin(const element& element, const format& format) -> layout_builder&
{
    const uint32_t index = static_cast<uint32_t>(nodes_.size());
    layout_node& node = nodes_.emplace_back();
    node.element = element;
    node.format = format;

    if (!open_.empty())
        layout_node::link(nodes_, open_.back(), index);

    open_.push_back(index);
    return *this;
}

constexpr auto layout_builder::begin(const format& format) -> layout_builder&
{
    element invisible{};
    invisible.sizing = size_fit();
    return begin(invisible, format);
}

constexpr auto layout_builder::add_element(const element& element) -> layout_builder&
{
    const uint32_t index = static_cast<uint32_t>(nodes_.size());
    nodes_.emplace_back().element = element;

    if (!open_.empty())
        layout_node::link(nodes_, open_.back(), index);

    return *this;
}

constexpr auto layout_builder::add_text(element element, std::string_view str, uint16_t font_px)
    -> layout_builder&
{
    element.text = static_cast<uint32_t>(texts_.size());  // 1-based (index 0 is the sentinel)
    texts_.push_back(text_run{ str, font_px });
    return add_element(element);
}

// Close the current container; if it was the outermost one, lay out its whole subtree. When
// `has_available`, the root is forced to `available` (a viewport) before space is distributed.
template <measurer Measure>
constexpr void layout_builder::close(bool has_available, dimension available, Measure& measure)
{
    if (open_.empty())
        return;

    const uint32_t index = open_.back();
    open_.pop_back();
    if (!open_.empty())
        return;

    fit_sizing(index, measure);
    if (has_available)
        nodes_[index].box.dimension = { detail::to_u16(available.width), detail::to_u16(available.height) };
    flex_sizing(index);
    position(index, 0, 0);
}

// Pass 1 (bottom-up): size each node to its content. A leaf's content is its measured
// intrinsic size (0 by default). A container's content is the sum of its children along the
// main axis (plus gaps + padding) and the max child on the cross axis.
template <measurer Measure>
constexpr void layout_builder::fit_sizing(uint32_t index, Measure& measure)
{
    layout_node& node = nodes_[index];

    if (node.first_child == layout_node::none)
    {
        const dimension content = measure(node.element);
        node.box.dimension.width = detail::to_u16(detail::resolve_axis(node.element.sizing.width, content.width));
        node.box.dimension.height = detail::to_u16(detail::resolve_axis(node.element.sizing.height, content.height));
        return;
    }

    for (uint32_t c = node.first_child; c != layout_node::none; c = nodes_[c].next_sibling)
        fit_sizing(c, measure);

    const bool horizontal = node.format.direction == direction::HORIZONTAL;

    int main_content = 0;
    int cross_content = 0;
    int child_count = 0;
    for (uint32_t c = node.first_child; c != layout_node::none; c = nodes_[c].next_sibling)
    {
        if (nodes_[c].element.floating)
            continue;  // overlay child: out of flow, no contribution to the parent's fit size
        const dimension d = nodes_[c].box.dimension;
        main_content += horizontal ? d.width : d.height;
        cross_content = std::max(cross_content, static_cast<int>(horizontal ? d.height : d.width));
        ++child_count;
    }
    if (child_count > 1)
        main_content += node.format.gap * (child_count - 1);

    main_content += detail::main_axis_padding(node.format);
    cross_content += detail::cross_axis_padding(node.format);

    const axis_sizing& main_sizing = horizontal ? node.element.sizing.width : node.element.sizing.height;
    const axis_sizing& cross_sizing = horizontal ? node.element.sizing.height : node.element.sizing.width;

    const int main_size = detail::resolve_axis(main_sizing, main_content);
    const int cross_size = detail::resolve_axis(cross_sizing, cross_content);

    node.box.dimension.width = detail::to_u16(horizontal ? main_size : cross_size);
    node.box.dimension.height = detail::to_u16(horizontal ? cross_size : main_size);
}

// Pass 2 (top-down): reconcile children with the container's main-axis inner size. Positive
// leftover grows GROW children; a deficit shrinks non-FIXED children down to their `min`.
// Cross-axis GROW children stretch to fill. Then recurse. Allocation-free.
constexpr void layout_builder::flex_sizing(uint32_t index) noexcept
{
    layout_node& node = nodes_[index];

    if (node.first_child != layout_node::none)
    {
        const bool horizontal = node.format.direction == direction::HORIZONTAL;

        const int inner_main = std::max(0,
            (horizontal ? node.box.dimension.width : node.box.dimension.height) - detail::main_axis_padding(node.format));
        const int inner_cross = std::max(0,
            (horizontal ? node.box.dimension.height : node.box.dimension.width) - detail::cross_axis_padding(node.format));

        auto main_of = [&](uint32_t c) {
            return horizontal ? nodes_[c].box.dimension.width : nodes_[c].box.dimension.height;
        };
        auto set_main = [&](uint32_t c, int v) {
            if (horizontal) nodes_[c].box.dimension.width = detail::to_u16(v);
            else            nodes_[c].box.dimension.height = detail::to_u16(v);
        };
        auto main_sizing = [&](uint32_t c) -> const axis_sizing& {
            return horizontal ? nodes_[c].element.sizing.width : nodes_[c].element.sizing.height;
        };

        int used = 0;
        int child_count = 0;
        for (uint32_t c = node.first_child; c != layout_node::none; c = nodes_[c].next_sibling)
        {
            if (nodes_[c].element.floating)
                continue;  // overlay child: out of flow
            used += main_of(c);
            ++child_count;
        }
        if (child_count > 1)
            used += node.format.gap * (child_count - 1);

        int leftover = inner_main - used;

        // Grow: hand surplus to GROW children (up to their max).
        while (leftover > 0)
        {
            int growers = 0;
            for (uint32_t c = node.first_child; c != layout_node::none; c = nodes_[c].next_sibling)
                if (!nodes_[c].element.floating && main_sizing(c).mode == size_mode::GROW && main_of(c) < main_sizing(c).max)
                    ++growers;
            if (growers == 0)
                break;

            const int share = std::max(1, leftover / growers);
            bool progressed = false;
            for (uint32_t c = node.first_child; c != layout_node::none && leftover > 0; c = nodes_[c].next_sibling)
            {
                if (nodes_[c].element.floating || main_sizing(c).mode != size_mode::GROW)
                    continue;
                const int room = static_cast<int>(main_sizing(c).max) - main_of(c);
                if (room <= 0)
                    continue;
                const int add = std::min({ share, room, leftover });
                set_main(c, main_of(c) + add);
                leftover -= add;
                progressed = true;
            }
            if (!progressed)
                break;
        }

        // Shrink: reclaim a deficit from non-FIXED children (down to their min).
        int deficit = -leftover;
        while (deficit > 0)
        {
            int shrinkers = 0;
            for (uint32_t c = node.first_child; c != layout_node::none; c = nodes_[c].next_sibling)
                if (!nodes_[c].element.floating && main_sizing(c).mode != size_mode::FIXED && main_of(c) > main_sizing(c).min)
                    ++shrinkers;
            if (shrinkers == 0)
                break;

            const int share = std::max(1, deficit / shrinkers);
            bool progressed = false;
            for (uint32_t c = node.first_child; c != layout_node::none && deficit > 0; c = nodes_[c].next_sibling)
            {
                if (nodes_[c].element.floating || main_sizing(c).mode == size_mode::FIXED)
                    continue;
                const int room = main_of(c) - static_cast<int>(main_sizing(c).min);
                if (room <= 0)
                    continue;
                const int sub = std::min({ share, room, deficit });
                set_main(c, main_of(c) - sub);
                deficit -= sub;
                progressed = true;
            }
            if (!progressed)
                break;
        }

        // Cross axis: GROW children stretch to fill the container.
        for (uint32_t c = node.first_child; c != layout_node::none; c = nodes_[c].next_sibling)
        {
            if (nodes_[c].element.floating)
                continue;
            const axis_sizing& cross = horizontal ? nodes_[c].element.sizing.height : nodes_[c].element.sizing.width;
            if (cross.mode != size_mode::GROW)
                continue;
            const int v = std::clamp(inner_cross, static_cast<int>(cross.min), static_cast<int>(cross.max));
            if (horizontal) nodes_[c].box.dimension.height = detail::to_u16(v);
            else            nodes_[c].box.dimension.width = detail::to_u16(v);
        }
    }

    for (uint32_t c = node.first_child; c != layout_node::none; c = nodes_[c].next_sibling)
        flex_sizing(c);
}

// Pass 3 (top-down): assign each node a position. Children advance along the main axis
// (leftover distributed per `justify`) and are offset on the cross axis per `alignment`.
constexpr void layout_builder::position(uint32_t index, uint16_t origin_x, uint16_t origin_y) noexcept
{
    layout_node& node = nodes_[index];
    node.box.x = origin_x;
    node.box.y = origin_y;

    if (node.first_child == layout_node::none)
        return;

    const bool horizontal = node.format.direction == direction::HORIZONTAL;
    const int inner_x = origin_x + node.format.padding.left;
    const int inner_y = origin_y + node.format.padding.top;
    const int inner_cross = std::max(0,
        (horizontal ? node.box.dimension.height : node.box.dimension.width) - detail::cross_axis_padding(node.format));

    // Main-axis leftover drives justify (leading offset + extra spacing between children). Floating
    // (overlay) children are out of flow — they don't count here and are placed absolutely below.
    int used = 0;
    int child_count = 0;
    for (uint32_t c = node.first_child; c != layout_node::none; c = nodes_[c].next_sibling)
    {
        if (nodes_[c].element.floating)
            continue;
        used += horizontal ? nodes_[c].box.dimension.width : nodes_[c].box.dimension.height;
        ++child_count;
    }
    if (child_count > 1)
        used += node.format.gap * (child_count - 1);

    const int inner_main = std::max(0,
        (horizontal ? node.box.dimension.width : node.box.dimension.height) - detail::main_axis_padding(node.format));
    const int leftover = std::max(0, inner_main - used);

    int leading = 0;
    int spacing = 0;
    if (child_count > 0)
    {
        switch (node.format.justify)
        {
            case justification::START:                                                 break;
            case justification::CENTER:  leading = leftover / 2;                       break;
            case justification::END:     leading = leftover;                           break;
            case justification::SPACE_BETWEEN:
                if (child_count > 1) spacing = leftover / (child_count - 1);
                break;
            case justification::SPACE_AROUND:
                spacing = leftover / child_count;
                leading = spacing / 2;
                break;
            case justification::SPACE_EVENLY:
                spacing = leftover / (child_count + 1);
                leading = spacing;
                break;
        }
    }

    int cursor = leading;
    for (uint32_t c = node.first_child; c != layout_node::none; c = nodes_[c].next_sibling)
    {
        // Overlay child: place its subtree at its absolute anchor (root space) and skip the flow.
        if (nodes_[c].element.floating)
        {
            position(c, nodes_[c].element.float_x, nodes_[c].element.float_y);
            continue;
        }
        const dimension d = nodes_[c].box.dimension;
        const int child_main = horizontal ? d.width : d.height;
        const int child_cross = horizontal ? d.height : d.width;

        int cross_off = 0;
        switch (node.format.alignment)
        {
            case alignment::CENTER:  cross_off = (inner_cross - child_cross) / 2; break;
            case alignment::RIGHT:
            case alignment::BOTTOM:  cross_off = inner_cross - child_cross;       break;
            default: /* TOP / LEFT — cross-axis start */                          break;
        }
        cross_off = std::max(0, cross_off);

        const int cx = horizontal ? inner_x + cursor : inner_x + cross_off;
        const int cy = horizontal ? inner_y + cross_off : inner_y + cursor;
        position(c, detail::to_u16(cx), detail::to_u16(cy));

        cursor += child_main + node.format.gap + spacing;
    }
}

constexpr auto layout_builder::end() -> layout_builder&
{
    no_measure measure;
    close(false, {}, measure);
    return *this;
}

constexpr auto layout_builder::end(const dimension& available) -> layout_builder&
{
    no_measure measure;
    close(true, available, measure);
    return *this;
}

template <measurer Measure>
constexpr auto layout_builder::end(Measure measure) -> layout_builder&
{
    close(false, {}, measure);
    return *this;
}

template <measurer Measure>
constexpr auto layout_builder::end(const dimension& available, Measure measure) -> layout_builder&
{
    close(true, available, measure);
    return *this;
}

// RAII container scope: closes its container (calls end()) when it goes out of scope, so
// begin/end can't get unbalanced. Move-only; created via layout_builder::container().
class layout_scope
{
    friend class layout_builder;
    layout_builder* builder_ = nullptr;
    constexpr explicit layout_scope(layout_builder& builder) noexcept : builder_(&builder) {}

public:
    layout_scope(const layout_scope&) = delete;
    layout_scope& operator=(const layout_scope&) = delete;
    layout_scope& operator=(layout_scope&&) = delete;
    constexpr layout_scope(layout_scope&& other) noexcept : builder_(other.builder_) { other.builder_ = nullptr; }
    constexpr ~layout_scope() { if (builder_) builder_->end(); }
};

constexpr auto layout_builder::container(const element& element, const format& format) -> layout_scope
{
    begin(element, format);
    return layout_scope{ *this };
}

constexpr auto layout_builder::container(const format& format) -> layout_scope
{
    begin(format);
    return layout_scope{ *this };
}

// Evaluate a captureless layout description into a fixed-size array whose extent is deduced
// from the layout itself — you don't state the node count. The description is a template
// parameter so it can run at compile time: once to count nodes, once to fill the array. The
// result is a std::array, so it can live in a constexpr/static object. Runs at runtime too.
//
//   constexpr auto ui = evaluate_layout<[](layout_builder& b) {
//       b.begin(panel, {.gap = 5, .direction = direction::HORIZONTAL})
//           .add_element(a).add_element(b).end();
//   }>();
//
// The closure must be captureless (compile-time layouts are static). For layouts driven by
// runtime data, use a layout_builder + nodes() directly.
template <auto Build>
[[nodiscard]] constexpr auto evaluate_layout()
{
    constexpr std::size_t count = [] {
        layout_builder builder;
        Build(builder);
        return builder.nodes().size();
    }();

    layout_builder builder;
    Build(builder);
    return builder.to_array<count>();
}

namespace detail
{

// A horizontal row of two fixed boxes with a gap, using the sizing helpers. Proves the whole
// pipeline is constant-evaluable and that the node pool is directly consumable (no emit pass).
consteval bool layout_self_test()
{
    element a{};
    a.sizing = size_fixed(10, 10);
    element b{};
    b.sizing = size_fixed(20, 10);

    layout_builder builder;
    builder.begin(format{ .gap = 5, .direction = direction::HORIZONTAL })
               .add_element(a)
               .add_element(b)
           .end();

    const auto nodes = builder.nodes();
    return nodes.size() == 3
        && nodes[1].box.x == 0
        && nodes[2].box.x == 15
        && nodes[0].box.dimension.width == 35;
}

static_assert(layout_self_test(), "layout: compile-time self-test failed");

// The array output path with a DEDUCED extent (inferred, not stated).
consteval auto layout_array_self_test()
{
    return evaluate_layout<[](layout_builder& builder) {
        element a{};
        a.sizing = size_fixed(10, 10);
        element b{};
        b.sizing = size_fixed(20, 10);
        builder.begin(format{ .gap = 5, .direction = direction::HORIZONTAL })
                   .add_element(a)
                   .add_element(b)
               .end();
    }>();
}

static_assert(layout_array_self_test().size() == 3, "layout: array extent should be deduced to 3");
static_assert(layout_array_self_test()[2].bounding_box.x == 15, "layout: array output self-test failed");

// Viewport + GROW + find()-by-id-hash + hit_test, exercising end(extent) and lookup.
consteval bool layout_viewport_self_test()
{
    using namespace literals;

    element root{};
    root.id = "root"_id;
    root.sizing = size_grow();
    element a{};
    a.id = "a"_id;
    a.sizing = { grow(), fixed(10) };
    element b{};
    b.id = "b"_id;
    b.sizing = { grow(), fixed(10) };

    layout_builder builder;
    builder.begin(root, format{ .gap = 10, .direction = direction::HORIZONTAL })
               .add_element(a)
               .add_element(b)
           .end(dimension{ 100, 40 });

    const layout_node* na = builder.find("a"_id);
    const layout_node* nb = builder.find("b"_id);
    return na != nullptr && nb != nullptr
        && builder.nodes()[0].box.dimension.width == 100
        && na->box.dimension.width == 45     // (100 - gap 10) / 2
        && nb->box.dimension.width == 45
        && na->box.x == 0
        && nb->box.x == 55                    // 45 + gap 10
        && builder.hit_test(60, 5) == nb      // topmost node at that point
        && builder.hit_test(5, 5) == na
        && builder.hit_test(200, 200) == nullptr;
}

static_assert(layout_viewport_self_test(), "layout: viewport/grow/find/hit_test self-test failed");

// Main-axis justification: space-between pushes children to the ends of a fixed container.
consteval bool layout_justify_self_test()
{
    element root{};
    root.sizing = size_fixed(100, 10);
    element a{};
    a.sizing = size_fixed(10, 10);
    element b{};
    b.sizing = size_fixed(10, 10);

    layout_builder builder;
    builder.begin(root, format{ .direction = direction::HORIZONTAL, .justify = justification::SPACE_BETWEEN })
               .add_element(a)
               .add_element(b)
           .end();

    const auto nodes = builder.nodes();
    return nodes[1].box.x == 0 && nodes[2].box.x == 90;   // 100 - 10
}

static_assert(layout_justify_self_test(), "layout: justify self-test failed");

// Overflow shrink: two FIT children (floor 20, min 5) shrink to fit a fixed-30 container.
consteval bool layout_shrink_self_test()
{
    element root{};
    root.sizing = size_fixed(30, 10);
    element a{};
    a.sizing = { fit(20), fixed(10) };   // wants 20 wide, min 0
    a.sizing.width.min = 5;
    element b{};
    b.sizing = { fit(20), fixed(10) };
    b.sizing.width.min = 5;

    layout_builder builder;
    builder.begin(root, format{ .direction = direction::HORIZONTAL })
               .add_element(a)
               .add_element(b)
           .end();

    const auto nodes = builder.nodes();
    return nodes[1].box.dimension.width == 15 && nodes[2].box.dimension.width == 15;  // 40 -> 30
}

static_assert(layout_shrink_self_test(), "layout: shrink self-test failed");

// Content-measurement hook: a FIT leaf sizes to its measured content.
consteval bool layout_measure_self_test()
{
    element text{};
    text.sizing = size_fit();

    layout_builder builder;
    builder.begin(format{ .direction = direction::VERTICAL })
               .add_element(text)
           .end([](const element&) -> dimension { return { 30, 12 }; });

    const auto nodes = builder.nodes();
    return nodes[1].box.dimension.width == 30 && nodes[1].box.dimension.height == 12;
}

static_assert(layout_measure_self_test(), "layout: measure self-test failed");

// RAII scope guard closes the container without an explicit end().
consteval bool layout_scope_self_test()
{
    element a{};
    a.sizing = size_fixed(10, 10);
    element b{};
    b.sizing = size_fixed(20, 10);

    layout_builder builder;
    {
        auto row = builder.container(format{ .gap = 5, .direction = direction::HORIZONTAL });
        builder.add_element(a);
        builder.add_element(b);
    }

    const auto nodes = builder.nodes();
    return nodes.size() == 3 && nodes[2].box.x == 15;
}

static_assert(layout_scope_self_test(), "layout: RAII scope self-test failed");

// Floating (overlay) child: placed at its absolute anchor, out of the parent's flow (a flowed
// sibling ignores it, and it doesn't grow the parent).
consteval bool layout_floating_self_test()
{
    element root{};
    root.sizing = size_fixed(100, 100);
    element flow{};
    flow.sizing = size_fixed(10, 10);
    element over{};
    over.sizing = size_fixed(20, 20);
    over.floating = true;
    over.float_x = 60;
    over.float_y = 40;

    layout_builder builder;
    builder.begin(root, format{ .direction = direction::VERTICAL })
               .add_element(flow)
               .add_element(over)
           .end();

    const layout_node* nf = &builder.nodes()[1];
    const layout_node* no = &builder.nodes()[2];
    return nf->box.x == 0 && nf->box.y == 0            // flow child at origin, unshifted by overlay
        && no->box.x == 60 && no->box.y == 40          // overlay at its absolute anchor
        && no->box.dimension.width == 20;
}

static_assert(layout_floating_self_test(), "layout: floating overlay self-test failed");

static_assert(make_id("panel").hash == fnv1a("panel"), "layout: id hash mismatch");

}  // namespace detail

}
