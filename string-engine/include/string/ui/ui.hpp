#pragma once

#include <concepts>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <string/core/layout.hpp>
#include <string/ui/interaction.hpp>
#include <string/ui/motion.hpp>
#include <string/ui/panel.hpp>
#include <string/ui/theme.hpp>
#include <string/ui/workspace.hpp>

// The fluent UI facade (brief 12 M0c, L4) — the authoring surface briefs 13-15 build on.
//
// It implements the conventions locked with the user on 2026-07-25:
//   * EXPLICIT NAMES AS IDENTITY — every element takes a name; it is the stable id that carries
//     hover/focus/drag/motion state across the per-frame immediate-mode rebuild, and it is
//     independent of the visible text (`.text(...)`). This is the fix for brief 05's silent
//     id-collision bug class.
//   * CALLBACK-FIRST EVENTS with poll sugar — `.on_click(fn)` is the primitive; `.clicked()` is
//     sugar over it. Callbacks are the superset a future retained layer needs.
//   * BINDING VIA TYPED ACCESSOR — `bind(handle)` (retained-safe) or `bind(obj, &T::field)`
//     (immediate-only escape hatch). See below; the handle form is deliberately NOT CVar-shaped.
//   * CLOSURES FOR NESTING — `.content([&](Ui& u){ ... })` so trees read as trees.
//   * FULL CLAY SIZING with inherited defaults, and a THEME every element inherits with
//     per-element override.
//
// It also owns the STRING SCRATCH ARENA. `layout_builder::add_text` takes a non-owning view, so
// every author previously had to keep its own frame-persistent storage and know that it must be a
// deque (a vector regrowth relocates SSO buffers and dangles every view taken so far). That rule is
// now enforced in one place instead of re-derived per author — `Ui::own()` is the only entry point.
namespace string::ui
{

// --- Binding -------------------------------------------------------------------------------------

// A typed accessor. Deliberately minimal: type erasure is only needed once a RETAINED tree exists,
// so it lands with retained mode rather than being paid for now.
template <typename T>
struct binding
{
    std::function<T()> get;
    std::function<void(T)> set;

    [[nodiscard]] bool valid() const { return static_cast<bool>(get) && static_cast<bool>(set); }
};

// A handle is anything that can be read and written by value. NOTE what this does NOT say: it never
// mentions CVar. The brief-15 material/light store handle satisfies it on the same terms, which is
// the point — the accessor must be handle-GENERIC, not CVar-shaped, or brief 15 would have to
// retrofit it. `CVar<T>` happens to satisfy it today.
template <typename H>
concept accessor_handle = requires(H& h) {
    { h.get() };
    h.set(h.get());
};

// Handle-backed bind — the RETAINED-SAFE canonical path. The handle outlives the frame, so a
// retained tree may keep this binding.
template <accessor_handle H>
[[nodiscard]] auto bind(H& handle)
{
    using T = std::decay_t<decltype(handle.get())>;
    return binding<T>{ [&handle] { return handle.get(); },
                       [&handle](T v) { handle.set(std::move(v)); } };
}

// Raw obj+member bind — the IMMEDIATE-ONLY escape hatch. Safe because it never outlives the build;
// a retained tree must NOT hold one (brief 12: "retained binds are restricted to handle-backed
// targets"). App state that must be retained-editable goes behind a store or a CVar.
template <typename O, typename T>
[[nodiscard]] binding<T> bind(O& obj, T O::* member)
{
    return binding<T>{ [&obj, member] { return obj.*member; },
                       [&obj, member](T v) { obj.*member = std::move(v); } };
}

class Ui;

// --- Element ---------------------------------------------------------------------------------------

// A fluent handle to one element being authored. Configuration is accumulated and emitted either by
// `.content(...)` (as a container, so children can be added inside the closure) or by the
// destructor (as a leaf). Chaining is for per-element config; nesting is closures.
class Element
{
public:
    Element(Ui& ui, id identity);
    ~Element();

    Element(const Element&) = delete;
    Element& operator=(const Element&) = delete;
    // Movable so the themed factories (text/button) can return a pre-configured element by
    // value. The moved-from element is marked emitted, so exactly one of the pair ever emits —
    // ordinary RAII move semantics, not a special case.
    Element(Element&& other) noexcept;
    Element& operator=(Element&&) = delete;   // a reference member makes assignment meaningless

    // --- Content -------------------------------------------------------------------------------
    // Visible text. Copied into the frame arena, so callers may pass a temporary — the dangling
    // text_run view that bit brief 05 is structurally impossible here.
    //
    // Applies the theme's text colour unless one was already set. That is safe HERE in a way it was
    // not in the constructor (which used to do it, and painted structural containers as a result):
    // `element.color` is the FILL for a container but the GLYPH colour for a text node, and only at
    // this point do we know which we have. Transparent text is never what anyone meant.
    Element& text(std::string_view content);
    Element& font(std::uint16_t px);
    // Word-wrap this element's text to whatever width the layout gives it, taking as many lines as
    // that needs. Opt-in: see layout.hpp — wrapping changes an element's height, so it is never
    // applied to text that did not ask for it.
    Element& wrap();

    // --- Visuals (default to the theme; these override just this element) -----------------------
    Element& color(string::color c);
    Element& stroke(string::color c, std::uint16_t width);
    Element& radius(std::uint16_t r);
    Element& shape(string::shape s);
    Element& rounded();               // theme radius + ROUNDED_RECTANGLE
    // Theme-derived container defaults: surface fill, stroke, rounded corners, padding and gap.
    // Pair with `element()` for a plain themed container or `element(name)` for an addressable one —
    // which is why this is a MODIFIER and not a factory: whether a container needs an id is the
    // author's call, and a factory would have had to pick one.
    Element& themed();
    Element& sweep(std::uint8_t s);   // radial cooldown sweep (0 = none)

    // --- Sizing (full Clay vocabulary) ---------------------------------------------------------
    Element& size(string::sizing s);
    Element& width(axis_sizing a);
    Element& height(axis_sizing a);
    Element& grow();
    Element& fit();
    Element& fixed(std::uint16_t w, std::uint16_t h);

    // --- Layout of children --------------------------------------------------------------------
    Element& row();       // HORIZONTAL
    Element& column();    // VERTICAL
    Element& gap(std::uint16_t g);
    Element& pad(std::uint16_t p);
    Element& pad(padding p);
    Element& align(alignment a);
    Element& justify(justification j);

    // --- Placement ------------------------------------------------------------------------------
    // Signed: an anchor left of or above the origin is a real position for scrolled and panned
    // content. See bounding_box in layout.hpp.
    Element& floating(int x, int y);
    Element& overlay();
    // Position within the PARENT rather than the root — for a container that computes its own
    // children's coordinates (see layout.hpp). Pairs with floating(x, y).
    Element& local();
    Element& z(std::uint8_t level);   // front-to-back order within the layer; higher = in front
    // Clip this element's subtree to its own box. See layout.hpp — this is what lets a scrolled or
    // panned container cut its content off mid-way instead of culling whole children at the edge.
    Element& clip();
    // Route the mouse wheel to this element while the cursor is anywhere in its subtree. See
    // layout.hpp — the container declares it, the hit test resolves it, and the widget reads
    // `interaction::wheel_for(id)`.
    Element& wheel();

    // --- Events (callback-first; poll is sugar over the same primitive) -------------------------
    Element& on_click(std::function<void()> fn);

    // Poll convenience for throwaway immediate-mode debug UI. Identical semantics to on_click —
    // both read the same resolved interaction state.
    [[nodiscard]] bool clicked() const;
    [[nodiscard]] bool hovered() const;
    [[nodiscard]] bool focused() const;
    [[nodiscard]] bool dragging() const;

    // --- Nesting ---------------------------------------------------------------------------------
    // Emits this element as a CONTAINER and authors its children inside the closure.
    Element& content(const std::function<void(Ui&)>& fn);

    [[nodiscard]] std::uint64_t id_hash() const { return element_.id.hash; }

private:
    void emit_leaf();
    void fire_events();

    Ui& ui_;
    struct element element_{};
    struct format format_{};
    std::string_view text_{};
    std::uint16_t font_px_ = 0;
    bool emitted_ = false;
};

// --- Panel ------------------------------------------------------------------------------------

// A floating, movable, resizable panel (brief 12 M1, L5).
//
// It is an ordinary element tree — a floating overlay container holding a title bar, a body, and a
// resize grip — plus one piece of state that cannot live in an immediate-mode tree: its rect. That
// lives in the `PanelStore` the Ui was constructed with, keyed by the panel's name hash.
//
// Authoring:
//     u.panel("stats").title("Stats").initial({ 40, 40, 320, 220 }).content([&](Ui& u) {
//         u.text("hello");
//     });
//
// The rect is applied on the SAME frame the drag is seen (update_panel runs inside content(),
// after begin_interaction has advanced the delta), so the panel tracks the cursor with no lag.
class Panel
{
public:
    Panel(Ui& ui, std::string_view name);

    // Visible caption. Independent of the name, which is identity — same rule as Element::text.
    Panel& title(std::string_view text);
    // Applied ONLY the first time this panel is seen; afterwards the stored rect wins. So an
    // author may compute a default position every frame without fighting a drag in progress.
    Panel& initial(panel_rect r);
    Panel& limits(panel_limits l);

    // Emits the panel and authors its body inside the closure.
    Panel& content(const std::function<void(Ui&)>& fn);

    // The live rect, valid after content(). Useful for anchoring things to the panel.
    [[nodiscard]] panel_rect rect() const;
    [[nodiscard]] bool moving() const;
    [[nodiscard]] bool resizing() const;

private:
    Ui& ui_;
    id id_{};
    id move_id_{};    // title bar
    id resize_id_{};  // corner grip
    panel_handles handles_{};
    std::string_view title_{};
    panel_rect initial_{ 40.0f, 40.0f, 320.0f, 220.0f };
    panel_limits limits_{};
};

// --- Card / WorkspaceView ------------------------------------------------------------------------

// A card DECLARATION. `content()` does not emit at the call site: in a single-pass immediate builder
// emission order IS tree structure, and the workspace — not the author — decides structure. So the
// body is stored and run during the workspace walk, which is forced by immediate mode plus
// data-driven placement rather than being a preference. See brief 12, "Authoring API".
class Card
{
public:
    Card(Ui& ui, std::uint64_t hash);

    Card& title(std::string_view text);
    // Registers the body. A card declared but not referenced by the workspace simply does not draw.
    Card& content(std::function<void(Ui&)> fn);

private:
    Ui& ui_;
    std::uint64_t hash_;
    std::string_view title_{};
};

// The frame-scoped handle over a `Workspace`. `content()` runs the declaration closure to collect
// cards, THEN walks the arrangement and emits it.
class WorkspaceView
{
public:
    WorkspaceView(Ui& ui, Workspace& ws) : ui_(ui), ws_(ws) {}

    WorkspaceView& content(const std::function<void(Ui&)>& declare);

private:
    Ui& ui_;
    Workspace& ws_;
};

// --- Ui ----------------------------------------------------------------------------------------

// LIFETIME (load-bearing — read before constructing one): a `Ui` OWNS the frame string arena, and
// `layout_builder` stores text as NON-OWNING views into it. So the Ui must outlive not just
// authoring but LAYOUT AND RECORD — i.e. the whole frame.
//
// Concretely: do NOT construct a Ui as a local inside the author callback. It dies when the callback
// returns, taking the arena with it, and every text node in the tree is left pointing at freed
// memory — including string literals, which are copied into the arena like everything else. Keep it
// beside the motion table in whatever object persists across frames (for the sandbox, the author
// closure). The layout-dump gate catches this instantly; the symptom is garbage glyph text.
class Ui
{
public:
    // Non-owning: the builder, interaction, motion table and panel store all outlive one frame's
    // authoring. `panels` is persistent for the same reason `motion` is — see panel.hpp.
    Ui(layout_builder& builder, const interaction& state, Motion& motion, const Theme& theme,
       PanelStore& panels)
        : builder_(builder), interaction_(state), motion_(motion), theme_(theme), panels_(panels)
    {
    }

    // THE HOST OWNS THE ROOT. The facade deliberately has no `root()` factory: the outermost
    // `end(available, measurer)` carries the screen size AND the text measurer, and the measurer is
    // a host concern (it holds the font atlas, and its type is a template parameter the engine
    // cannot name here). So the host opens a root container, authors through `Ui` inside it, and
    // closes it. `content()` always closes with a plain `end()`, which is correct for every
    // NON-root element — an element that is itself outermost would never receive the available
    // size, and its grow/fit would resolve against nothing.

    // Start a frame: clears the arena and advances motion. Call once, before authoring.
    void begin_frame();
    // End a frame: ages out motion entries for elements that were not authored.
    void end_frame();

    // --- Element factories -----------------------------------------------------------------------
    // `name` is IDENTITY, not the visible text. It must be stable across frames and unique among
    // siblings. IDENTITY IS THE ONLY THING THE TWO `element` OVERLOADS DIFFER BY, which is why they
    // share a name: the presence of a name at the call site IS the distinction, so there is nothing
    // extra to memorise.
    //
    // ID-LESSNESS IS MEANINGFUL, not an oversight: `hit_test` resolves THROUGH an id-less node to
    // the nearest id'd ancestor, so "no name" is precisely how you say "this node is decoration,
    // not an interaction target". A button's caption MUST be id-less — name it and the caption
    // becomes the hover target instead of the button. So names govern INTERACTIVE elements;
    // structure and decoration go unnamed.
    //
    // There is deliberately NO `label(name, text)` factory. Text that carries identity is the rare
    // case AND the usual mistake (see above), so it composes explicitly instead:
    // `u.element("link").text("Click me")`. Two adjacent string parameters would also give the
    // reader no way to tell identity from content.
    //
    // Deliberately NOT [[nodiscard]]: an element emits itself in its DESTRUCTOR, so discarding the
    // return value is the normal way to author a leaf (`u.text("hi");`). Marking these nodiscard
    // warns on correct code and trains the author to write noise to silence it.
    Element element();                        // structural / decorative
    Element element(std::string_view name);   // addressable: hover, focus, motion, drag
    Element text(std::string_view content);   // sugar for element().text(content)
    Element button(std::string_view name);    // themed, motion-driven hover/press

    // --- Panels ------------------------------------------------------------------------------------
    Panel panel(std::string_view name);

    // --- Workspace ---------------------------------------------------------------------------------
    WorkspaceView workspace(Workspace& ws);
    // Declares a dockable card. Legal only inside `workspace(...).content(...)`; elsewhere it is a
    // no-op, because a card with no workspace has nowhere to be placed.
    Card card(std::uint64_t hash);
    Card card(id identity) { return card(identity.hash); }

    // Declaration lookup, used by the workspace walk. Public because the walk lives outside the
    // class; deliberately NOT exposing the declaration struct itself.
    [[nodiscard]] bool card_declared(std::uint64_t hash) const;
    [[nodiscard]] std::string_view card_title(std::uint64_t hash) const;
    // Runs the declared body against this Ui. No-op when the card was not declared this frame.
    void emit_card(std::uint64_t hash);

    // --- The scratch arena -----------------------------------------------------------------------
    // Copies `s` into frame-persistent storage and returns a view valid until the next
    // begin_frame(). A deque, NOT a vector: appends must never relocate earlier strings.
    [[nodiscard]] std::string_view own(std::string s);
    [[nodiscard]] std::string_view own(std::string_view s) { return own(std::string(s)); }

    [[nodiscard]] const Theme& theme() const { return theme_; }
    [[nodiscard]] const interaction& interaction_state() const { return interaction_; }
    [[nodiscard]] Motion& motion() { return motion_; }
    [[nodiscard]] layout_builder& builder() { return builder_; }

    [[nodiscard]] PanelStore& panels() { return panels_; }

    // Per-widget persistent scratch, keyed by the widget's stable id: a combo's open/closed flag, a
    // drag-value's value-at-press. Widgets are rebuilt every frame, so anything that must outlive
    // the rebuild needs a home, and `Ui` is the one object that already persists across frames.
    //
    // Deliberately one weak type rather than a typed store per widget: this holds a handful of
    // scalars, and a `variant`/type-erased store would be more machinery than the problem.
    [[nodiscard]] double& widget_state(std::uint64_t id, double initial = 0.0);

    // --- Post-layout box observation ---------------------------------------------------------
    // Registers `id` for observation and returns the box layout gave it LAST frame — empty until it
    // has been laid out once.
    //
    // This is the sanctioned form of the thing `interaction::hovered_box` deliberately is not: an
    // OPT-IN, per-id cache rather than a map of every element. A widget needs this when it must know
    // its own extent CONTINUOUSLY rather than while the pointer is on it — a scroll area clamping to
    // its content height is the case that forced it, since content height is exactly what the layout
    // computes and the author does not know.
    //
    // ONE FRAME STALE, unavoidably: sizes exist only after layout, and the widget that needs them is
    // rebuilt before it. That is fine for an EXTENT (it changes when content changes, not per frame)
    // and would not be for a position the pointer chases — which is what hovered_box is for.
    [[nodiscard]] bounding_box observed_box(std::uint64_t id);
    // Host seam, called AFTER layout resolves: caches the boxes of the registered ids. Same seam as
    // `Workspace::observe`, and for the same reason.
    void observe();

    // A focused text field asks for keystrokes to stop reaching gameplay. A REQUEST, not an action:
    // capture is the host's decision, exactly as with the game/UI mode requests on `interaction`.
    // Cleared each frame, so it is true only while something actually wants it.
    void request_text_capture() noexcept { text_capture_ = true; }
    [[nodiscard]] bool wants_text_capture() const noexcept { return text_capture_; }

private:
    friend class Element;
    friend class Panel;

    layout_builder& builder_;
    const interaction& interaction_;
    Motion& motion_;
    const Theme& theme_;
    PanelStore& panels_;
    std::deque<std::string> arena_;

    // Click-to-raise. While a Panel authors its content, `authoring_panel_` names it and every
    // Element created checks whether IT is the element pressed this frame. That is what makes
    // clicking a BUTTON inside a buried panel raise the panel: ancestry is known here, at authoring
    // time, without the tree needing to be walked or `interaction` learning what a panel is.
    // Ids are stable across frames, so matching this frame's authored id against a press resolved
    // from last frame's tree is correct.
    std::uint64_t authoring_panel_ = 0;
    bool panel_pressed_ = false;

    // Card declarations collected during a workspace's declare closure, consumed by its walk.
    struct CardDecl
    {
        std::uint64_t hash = 0;
        std::string_view title;
        std::function<void(Ui&)> body;
    };
    std::vector<CardDecl> cards_;
    bool declaring_cards_ = false;

    std::unordered_map<std::uint64_t, double> widget_state_;
    std::unordered_map<std::uint64_t, bounding_box> observed_;
    bool text_capture_ = false;

    friend class Card;
    friend class WorkspaceView;
    [[nodiscard]] const CardDecl* find_card(std::uint64_t hash) const;
};

}  // namespace string::ui
