#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <span>
#include <vector>
#include <string_view>

#include <string/ui/ui.hpp>

// The engine widget kit (brief 13).
//
// THE LINE, from brief 12: ENGINE widgets encode INTERACTION, app widgets encode MEANING. A checkbox
// is interaction and lives here; an inventory cell's rarity border and cooldown sweep are meaning and
// stay in the app.
//
// Every widget follows the conventions locked in brief 12 and does NOT re-decide them: an explicit
// name as identity, `bind(...)` for data, callbacks with poll sugar, chainable config, theme
// defaults with per-element override. Each emits in its DESTRUCTOR, like `Element`, so a widget is
// an ordinary statement:
//
//     u.checkbox("wireframe", bind(cv_wireframe)).label("Wireframe");
//
// WIDGET STATE lives in `Ui::widget_state(id)`: widgets are rebuilt every frame, so anything that
// must survive the rebuild (a combo's open flag, a drag's value-at-press) is keyed by the widget's
// stable id rather than held in the widget object, which does not outlive the statement.
namespace string::ui
{

// --- Checkbox --------------------------------------------------------------------------------------

// A toggle. Clicking anywhere on the row flips it, so the label is part of the target rather than
// decoration beside it — the box alone is a small thing to hit.
class Checkbox
{
public:
    Checkbox(Ui& ui, std::string_view name, binding<bool> value);
    ~Checkbox();

    Checkbox(const Checkbox&) = delete;
    Checkbox& operator=(const Checkbox&) = delete;

    Checkbox& label(std::string_view text);
    Checkbox& on_change(std::function<void(bool)> fn);
    [[nodiscard]] bool changed() const { return changed_; }

private:
    Ui& ui_;
    id id_{};
    binding<bool> value_;
    std::string_view label_{};
    std::function<void(bool)> on_change_{};
    bool changed_ = false;
    bool emitted_ = false;
    void emit();
};

// --- Drag value ------------------------------------------------------------------------------------

// A numeric scrub: press and drag horizontally to change the value.
//
// RELATIVE, deliberately. It needs no knowledge of where it ended up on screen, which an absolute
// slider does — a slider must map a cursor position onto its own track, and a track's rect does not
// exist until after layout. Drag-value therefore lands first and a true slider follows the
// post-layout box cache.
//
// The value at press is captured in `Ui::widget_state`, so the delta is applied to a fixed origin
// rather than accumulated — the same rule that keeps panel drag and splitter drag drift-free.
class DragValue
{
public:
    DragValue(Ui& ui, std::string_view name, binding<float> value);
    ~DragValue();

    DragValue(const DragValue&) = delete;
    DragValue& operator=(const DragValue&) = delete;

    DragValue& label(std::string_view text);
    DragValue& range(float lo, float hi);
    DragValue& step(float per_pixel);        // value change per pixel dragged
    DragValue& precision(int decimals);
    DragValue& on_change(std::function<void(float)> fn);
    [[nodiscard]] bool changed() const { return changed_; }

private:
    Ui& ui_;
    id id_{};
    binding<float> value_;
    std::string_view label_{};
    float lo_ = 0.0f;
    float hi_ = 1.0f;
    float step_ = 0.0f;   // 0 = derive from the range
    int decimals_ = 2;
    std::function<void(float)> on_change_{};
    bool changed_ = false;
    bool emitted_ = false;
    void emit();
};

// --- Slider ----------------------------------------------------------------------------------------

// An ABSOLUTE slider: the value follows the cursor's position along the track, so pressing at 80%
// jumps to 80% and dragging tracks under your finger.
//
// That is the only difference from `DragValue`, and it is entirely down to one thing: a slider must
// map a cursor position onto its OWN rect, which it gets from `interaction::hovered_box`. Because
// the box arrives from the hit test, the slider only knows where it is while the pointer is on it —
// which is exactly when it needs to know.
class Slider
{
public:
    Slider(Ui& ui, std::string_view name, binding<float> value);
    ~Slider();

    Slider(const Slider&) = delete;
    Slider& operator=(const Slider&) = delete;

    Slider& label(std::string_view text);
    Slider& range(float lo, float hi);
    Slider& precision(int decimals);
    Slider& width(std::uint16_t px);
    Slider& on_change(std::function<void(float)> fn);
    [[nodiscard]] bool changed() const { return changed_; }

private:
    Ui& ui_;
    id id_{};
    binding<float> value_;
    std::string_view label_{};
    float lo_ = 0.0f;
    float hi_ = 1.0f;
    int decimals_ = 2;
    std::uint16_t width_ = 160;
    std::function<void(float)> on_change_{};
    bool changed_ = false;
    bool emitted_ = false;
    void emit();
};

// --- Text field ------------------------------------------------------------------------------------

// A single-line editable string.
//
// Only the FOCUSED field consumes typed text, which is why focus is a first-class part of the
// interaction state rather than a widget-local flag: with two fields on screen, exactly one must
// receive the keystrokes, and the arbitration belongs above any single widget.
//
// SCOPE, deliberately: the caret sits at the END and edits append or backspace. Caret movement with
// the arrow keys collides with directional FOCUS NAV — left/right would have to mean "move the
// caret" while a field is focused and "change focus" otherwise, and that arbitration is a real modal
// decision (the same class as `text_capture`), not something to slip in with the widget.
class TextField
{
public:
    TextField(Ui& ui, std::string_view name, binding<std::string> value);
    ~TextField();

    TextField(const TextField&) = delete;
    TextField& operator=(const TextField&) = delete;

    TextField& label(std::string_view text);
    TextField& placeholder(std::string_view text);
    TextField& width(std::uint16_t px);
    // Force this field to behave as focused this frame.
    //
    // For a surface that OWNS the field and knows it should be live — a modal console or a chat bar
    // that just opened. Focus is otherwise earned by a click or by nav, and there is deliberately no
    // way to push focus through `interaction`: it is resolved post-layout from the tree, so a
    // programmatic write would be a second source of truth for the same thing.
    //
    // The owner asserting it is the honest framing. Two fields both forcing focus is an authoring
    // error of exactly the same class as two elements sharing a name.
    TextField& focus(bool forced);
    TextField& on_change(std::function<void(const std::string&)> fn);
    TextField& on_submit(std::function<void(const std::string&)> fn);
    [[nodiscard]] bool changed() const { return changed_; }

private:
    Ui& ui_;
    id id_{};
    binding<std::string> value_;
    std::string_view label_{};
    std::string_view placeholder_{};
    std::uint16_t width_ = 180;
    bool force_focus_ = false;
    std::function<void(const std::string&)> on_change_{};
    std::function<void(const std::string&)> on_submit_{};
    bool changed_ = false;
    bool emitted_ = false;
    void emit();
};

// --- Combo -----------------------------------------------------------------------------------------

// A one-of-N chooser. The open list expands IN FLOW beneath the button rather than floating over the
// content: a floating popup has to be positioned at the button's screen rect, which — like the
// slider's track — does not exist until after layout. Inline is honest for v1 and needs no geometry.
class Combo
{
public:
    Combo(Ui& ui, std::string_view name, binding<int> index);
    ~Combo();

    Combo(const Combo&) = delete;
    Combo& operator=(const Combo&) = delete;

    Combo& label(std::string_view text);
    Combo& options(std::span<const std::string_view> opts);
    Combo& on_change(std::function<void(int)> fn);
    [[nodiscard]] bool changed() const { return changed_; }

private:
    Ui& ui_;
    id id_{};
    binding<int> index_;
    std::string_view label_{};
    std::span<const std::string_view> options_{};
    std::function<void(int)> on_change_{};
    bool changed_ = false;
    bool emitted_ = false;
    void emit();
};

// --- Collapsible -----------------------------------------------------------------------------------

// A section header that folds its body away. The open/closed flag is keyed by the widget's stable id
// in `Ui::widget_state`, so it survives the per-frame rebuild without the author holding it.
class Collapsible
{
public:
    Collapsible(Ui& ui, std::string_view name);

    Collapsible& label(std::string_view text);
    Collapsible& open(bool initially);   // applies only the first time this section is seen
    // Emits the header, and the body only when open. Returns whether the body was emitted, so an
    // author can skip expensive work behind a closed section entirely.
    bool content(const std::function<void(Ui&)>& fn);
    [[nodiscard]] bool is_open() const;

private:
    Ui& ui_;
    id id_{};
    std::string_view label_{};
    bool initial_ = true;
};

// --- Tabs ------------------------------------------------------------------------------------------

// A tab BAR bound to a selected index. The body is the author's — they already hold the index, so
// branching on it is clearer than a callback that hands the same number back.
//
// This is the standalone counterpart to brief 12's workspace tab bars, for tabs INSIDE a card rather
// than tabs that dock and tear off.
class Tabs
{
public:
    Tabs(Ui& ui, std::string_view name, binding<int> index);
    ~Tabs();

    Tabs(const Tabs&) = delete;
    Tabs& operator=(const Tabs&) = delete;

    Tabs& options(std::span<const std::string_view> labels);
    Tabs& on_change(std::function<void(int)> fn);
    // Emits the bar, then the body. Splitting them would let an author emit the body first and get
    // a frame-late tab switch.
    Tabs& content(const std::function<void(Ui&)>& fn);

private:
    Ui& ui_;
    id id_{};
    binding<int> index_;
    std::span<const std::string_view> labels_{};
    std::function<void(int)> on_change_{};
    bool emitted_ = false;
    void emit_bar();
};

// --- Colour picker ---------------------------------------------------------------------------------

// A swatch plus per-channel sliders.
//
// RGB only for now. An HSV square needs a 2D field — mapping a cursor onto BOTH axes of its own rect
// — which `hovered_box` can actually support, but it is a distinct control rather than a variation
// on this one, so it earns its own step rather than being bolted on here.
class ColorPicker
{
public:
    ColorPicker(Ui& ui, std::string_view name, binding<string::color> value);
    ~ColorPicker();

    ColorPicker(const ColorPicker&) = delete;
    ColorPicker& operator=(const ColorPicker&) = delete;

    ColorPicker& label(std::string_view text);
    ColorPicker& alpha(bool show);   // include an alpha channel row
    ColorPicker& on_change(std::function<void(string::color)> fn);
    [[nodiscard]] bool changed() const { return changed_; }

private:
    Ui& ui_;
    id id_{};
    binding<string::color> value_;
    std::string_view label_{};
    bool alpha_ = false;
    std::function<void(string::color)> on_change_{};
    bool changed_ = false;
    bool emitted_ = false;
    void emit();
};

// --- Menu bar --------------------------------------------------------------------------------------

// The items of one open menu. Handed to a menu's closure, which runs ONLY while that menu is open —
// so a menu whose items are expensive to build costs nothing closed.
class Menu
{
public:
    Menu(Ui& ui, std::string_view base, std::size_t index, bool& chose)
        : ui_(ui), base_(base), index_(index), chose_(chose)
    {
    }

    Menu& item(std::string_view label, std::function<void()> on_click);
    // A checkable row bound to a flag — the common case for a debug menu (`View > Profiler HUD`).
    Menu& toggle(std::string_view label, binding<bool> value);
    Menu& separator();

private:
    Ui& ui_;
    std::string base_;
    std::size_t index_ = 0;
    std::size_t row_ = 0;   // per-item id counter, so labels need not be unique
    // Set when an item is activated, so the bar can dismiss itself. See MenuBar::emit.
    bool& chose_;
};

// A File/Edit/View-style bar with drop-down menus (brief 14 M1).
//
// WHY THIS IS NOT A `Combo` VARIANT: a combo expands IN FLOW, and its header explains why — a popup
// must be positioned at its button's screen rect, "which does not exist until after layout". That is
// no longer true: `Ui::observed_box()` (added for the scroll area) is exactly that anchor. Menus also
// want interaction a combo does not have — hover-to-switch between open menus, and click-outside to
// dismiss.
//
// IT IS CHROME, NOT A PANEL (brief 12's model: composed elements, like the HUD and tooltips). The bar
// is placed as a floating overlay at the top of the screen rather than flowed, so it does not depend
// on being authored before anything else.
class MenuBar
{
public:
    MenuBar(Ui& ui, std::string_view name);
    ~MenuBar();

    MenuBar(const MenuBar&) = delete;
    MenuBar& operator=(const MenuBar&) = delete;

    // Declares a menu. `items` runs during emission, and only when this menu is open.
    MenuBar& menu(std::string_view label, std::function<void(Menu&)> items);

    [[nodiscard]] std::uint16_t height() const;   // so an author can inset content below the bar

private:
    Ui& ui_;
    id id_{};
    struct Entry
    {
        std::string_view label;
        std::function<void(Menu&)> items;
    };
    std::vector<Entry> menus_;
    bool emitted_ = false;
    void emit();
};

// --- Scroll area -----------------------------------------------------------------------------------

// A clipped viewport over content taller than itself.
//
// This is the general case the table and tree solve narrowly. They are VIRTUALIZED — they know their
// content is uniform rows, so they can compute which ones are visible and build only those. A scroll
// area's content is arbitrary, so it cannot: it builds everything and CLIPS, which is why it needs
// the scissor primitive and they did not.
//
// It also needs one thing no other widget has needed: its own content's HEIGHT, which is precisely
// what the layout computes and the author does not know. That arrives via `Ui::observed_box` and is
// therefore one frame stale — visible only as the scroll EXTENT settling on the second frame after
// content changes size, never as a lagging scroll position.
//
// FIXED SIZE, like `Graph`: the viewport is the thing being scrolled, so its extent is the author's
// statement of intent rather than something to derive.
class ScrollArea
{
public:
    ScrollArea(Ui& ui, std::string_view name);

    ScrollArea& size(std::uint16_t w, std::uint16_t h);
    ScrollArea& speed(float px_per_notch);

    // Emits the viewport and authors the scrolled content inside it.
    ScrollArea& content(const std::function<void(Ui&)>& fn);

    [[nodiscard]] float offset() const;

private:
    Ui& ui_;
    id id_{};
    std::uint16_t w_ = 260;
    std::uint16_t h_ = 200;
    float speed_ = 48.0f;
};

// --- Table -----------------------------------------------------------------------------------------

struct table_column
{
    std::string_view label;
    std::uint16_t width = 120;
};

// A virtualized table.
//
// ADAPTER-BACKED with loop-like ergonomics (locked in brief 12): you give a row COUNT and a per-cell
// renderer, and the table decides how often to call you. Only the rows actually on screen are built,
// which is the direct lever on immediate-mode CPU cost — a 10,000-row table costs the same as a
// 20-row one.
//
// SORTING AND FILTERING ARE THE AUTHOR'S. The table owns no data, so it cannot reorder it; it
// reports which column was clicked and in which direction, and the author sorts what it owns. That
// is what "adapter-backed" means, and it is also the only version that stays correct when the data
// is a view over something else.
//
// The viewport is sized in ROWS rather than measured from the laid-out rect: a virtualized list needs
// to know how many rows fit BEFORE it builds them, and a container's height does not exist until
// after layout. Asking the author for a row budget avoids a frame-late viewport.
class Table
{
public:
    Table(Ui& ui, std::string_view name);
    ~Table();

    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    Table& columns(std::span<const table_column> cols);
    Table& visible_rows(std::size_t n);          // viewport height, in rows
    Table& row_height(std::uint16_t px);
    Table& selected(binding<int> index);         // -1 = nothing selected
    Table& on_sort(std::function<void(std::size_t column, bool ascending)> fn);
    Table& on_activate(std::function<void(std::size_t row)> fn);   // double-purpose: click a row
    // Fired during emission for the row under the cursor, if any. For a table that DRIVES something
    // else while you sweep it — the scene inspector highlights the hovered draw's bounds in-world —
    // where waiting for a click would make the tool useless. Callback rather than a `hovered_row()`
    // poll because the answer only exists during emission, and a getter would invite reading it
    // before the rows have been built.
    Table& on_hover(std::function<void(std::size_t row)> fn);

    // Emits header, viewport and scrollbar. `render` is called ONLY for visible cells.
    Table& rows(std::size_t count,
                const std::function<void(Ui&, std::size_t row, std::size_t column)>& render);

    [[nodiscard]] std::size_t first_visible() const;

private:
    Ui& ui_;
    id id_{};
    std::span<const table_column> columns_{};
    std::size_t visible_ = 12;
    std::uint16_t row_h_ = 22;
    binding<int> selected_{};
    std::function<void(std::size_t, bool)> on_sort_{};
    std::function<void(std::size_t)> on_activate_{};
    std::function<void(std::size_t)> on_hover_{};
    bool emitted_ = false;

    // Column widths live in widget_state keyed by column, so a resize survives the rebuild.
    [[nodiscard]] std::uint16_t width_of(std::size_t col) const;
};

// --- Tree ------------------------------------------------------------------------------------------

// A virtualized tree.
//
// ADAPTER-BACKED like the table, but over a HIERARCHY: you supply the roots and a way to ask any node
// for its children, and the tree walks it. Nodes are addressed by an opaque `std::uint64_t` key —
// whatever identifies a node in the author's own data — because expand state has to survive the
// per-frame rebuild and a pointer or an index into a mutating container would not.
//
// COLLAPSED SUBTREES ARE NEVER WALKED, which is the important complexity property: a tree over a
// million nodes with everything folded costs its root count. The expanded set IS flattened each frame
// (that is what makes the row window addressable), so cost tracks what is OPEN, not what exists.
class Tree
{
public:
    Tree(Ui& ui, std::string_view name);

    Tree& visible_rows(std::size_t n);
    Tree& row_height(std::uint16_t px);
    Tree& indent(std::uint16_t px);
    Tree& selected(binding<std::uint64_t> key);   // 0 = nothing selected
    Tree& roots(std::span<const std::uint64_t> keys);
    // How to walk: how many children a node has, and the key of its i-th child.
    Tree& children(std::function<std::size_t(std::uint64_t)> count,
                   std::function<std::uint64_t(std::uint64_t, std::size_t)> child);
    Tree& on_activate(std::function<void(std::uint64_t)> fn);

    // Emits the visible window. `render` is called only for nodes actually on screen.
    Tree& nodes(const std::function<void(Ui&, std::uint64_t key, std::size_t depth)>& render);

    [[nodiscard]] bool is_expanded(std::uint64_t key) const;
    void set_expanded(std::uint64_t key, bool open);

private:
    Ui& ui_;
    id id_{};
    std::size_t visible_ = 12;
    std::uint16_t row_h_ = 22;
    std::uint16_t indent_ = 14;
    binding<std::uint64_t> selected_{};
    std::span<const std::uint64_t> roots_{};
    std::function<std::size_t(std::uint64_t)> count_{};
    std::function<std::uint64_t(std::uint64_t, std::size_t)> child_{};
    std::function<void(std::uint64_t)> on_activate_{};
};

// --- Graph -----------------------------------------------------------------------------------------

struct graph_edge
{
    std::uint64_t from = 0;
    std::uint64_t to = 0;
};

// A node/edge canvas with layered auto-layout.
//
// Built for the render-graph DAG (brief 14 wires passes to nodes and resource dependencies to edges),
// so the defaults suit a DAG: nodes are placed in columns by TOPOLOGICAL DEPTH, and edges route
// orthogonally between them.
//
// EDGES ARE AXIS-ALIGNED RECTANGLES, not curves. The renderer draws rounded rects and glyphs; there
// is no line primitive and no rotation. Manhattan routing is what those primitives can express — and
// it is what layered DAG viewers conventionally use anyway, so this is a fit rather than a
// compromise.
//
// Node positions are PARENT-RELATIVE (`float_local`), so the canvas never needs to know where it
// landed on screen. Manual repositioning is persisted by key.
class Graph
{
public:
    Graph(Ui& ui, std::string_view name);

    Graph& size(std::uint16_t w, std::uint16_t h);
    Graph& node_size(std::uint16_t w, std::uint16_t h);
    Graph& spacing(std::uint16_t x, std::uint16_t y);
    Graph& selected(binding<std::uint64_t> key);
    Graph& edges(std::span<const graph_edge> e);
    Graph& on_activate(std::function<void(std::uint64_t)> fn);

    // `keys` are the nodes, in any order; `render` draws the body of each.
    Graph& nodes(std::span<const std::uint64_t> keys,
                 const std::function<void(Ui&, std::uint64_t key)>& render);

private:
    Ui& ui_;
    id id_{};
    std::uint16_t w_ = 520;
    std::uint16_t h_ = 300;
    std::uint16_t node_w_ = 110;
    std::uint16_t node_h_ = 34;
    std::uint16_t gap_x_ = 60;
    std::uint16_t gap_y_ = 14;
    binding<std::uint64_t> selected_{};
    std::span<const graph_edge> edges_{};
    std::function<void(std::uint64_t)> on_activate_{};
};

// --- Factories -------------------------------------------------------------------------------------
// Free functions rather than `Ui` methods: the widget kit is a layer ABOVE the facade, and folding
// every widget into `Ui` would make that class grow without bound as briefs 13-15 add to the set.
[[nodiscard]] Checkbox checkbox(Ui& u, std::string_view name, binding<bool> value);
[[nodiscard]] DragValue drag_value(Ui& u, std::string_view name, binding<float> value);
[[nodiscard]] Slider slider(Ui& u, std::string_view name, binding<float> value);
[[nodiscard]] TextField text_field(Ui& u, std::string_view name, binding<std::string> value);
[[nodiscard]] Combo combo(Ui& u, std::string_view name, binding<int> index);
[[nodiscard]] Collapsible collapsible(Ui& u, std::string_view name);
[[nodiscard]] MenuBar menu_bar(Ui& u, std::string_view name);
[[nodiscard]] ScrollArea scroll_area(Ui& u, std::string_view name);
[[nodiscard]] Table table(Ui& u, std::string_view name);
[[nodiscard]] Tree tree(Ui& u, std::string_view name);
[[nodiscard]] Graph graph(Ui& u, std::string_view name);
[[nodiscard]] Tabs tabs(Ui& u, std::string_view name, binding<int> index);
[[nodiscard]] ColorPicker color_picker(Ui& u, std::string_view name, binding<string::color> value);

}  // namespace string::ui
