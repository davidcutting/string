#pragma once

#include <concepts>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <string/core/layout.hpp>
#include <string/ui/interaction.hpp>
#include <string/ui/motion.hpp>
#include <string/ui/theme.hpp>

// The fluent UI facade (brief 12 M0c, L4) — the authoring surface briefs 13-15 build on.
//
// It implements the conventions locked with the user on 2026-07-25:
//   * EXPLICIT NAMES AS IDENTITY — every element takes a name; it is the stable id that carries
//     hover/focus/drag/motion state across the per-frame immediate-mode rebuild, and it is
//     independent of the visible label (`.label(...)`). This is the fix for brief 05's silent
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
    // Movable so the themed factories (panel/label/button) can return a pre-configured element by
    // value. The moved-from element is marked emitted, so exactly one of the pair ever emits —
    // ordinary RAII move semantics, not a special case.
    Element(Element&& other) noexcept;
    Element& operator=(Element&&) = delete;   // a reference member makes assignment meaningless

    // --- Content -------------------------------------------------------------------------------
    // Visible text. Copied into the frame arena, so callers may pass a temporary — the dangling
    // text_run view that bit brief 05 is structurally impossible here.
    Element& label(std::string_view text);
    Element& font(std::uint16_t px);

    // --- Visuals (default to the theme; these override just this element) -----------------------
    Element& color(string::color c);
    Element& stroke(string::color c, std::uint16_t width);
    Element& radius(std::uint16_t r);
    Element& shape(string::shape s);
    Element& rounded();               // theme radius + ROUNDED_RECTANGLE
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
    Element& floating(std::uint16_t x, std::uint16_t y);
    Element& overlay();

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
    // Non-owning: the builder, interaction and motion table all outlive one frame's authoring.
    Ui(layout_builder& builder, const interaction& state, Motion& motion, const Theme& theme)
        : builder_(builder), interaction_(state), motion_(motion), theme_(theme)
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
    // `name` is IDENTITY, not the label. It must be stable across frames and unique among siblings.
    [[nodiscard]] Element element(std::string_view name);
    [[nodiscard]] Element panel(std::string_view name);    // themed surface: fill, stroke, radius, pad
    [[nodiscard]] Element label(std::string_view name, std::string_view text);
    [[nodiscard]] Element button(std::string_view name);   // themed, motion-driven hover/press

    // --- Anonymous (id-less) elements ------------------------------------------------------------
    // ID-LESSNESS IS MEANINGFUL, not an oversight: `hit_test` resolves THROUGH an id-less node to
    // the nearest id'd ancestor, so "no id" is precisely how you say "this node is decoration, not
    // an interaction target". A button's caption MUST be id-less — give it a name and the caption
    // becomes the hover target instead of the button.
    //
    // So "explicit names as identity" governs INTERACTIVE elements; pure structure and decoration
    // use these. Naming everything would be worse, not better.
    [[nodiscard]] Element box();                           // structural / decorative element
    [[nodiscard]] Element text(std::string_view content);  // decorative text (captions, rows)

    // --- The scratch arena -----------------------------------------------------------------------
    // Copies `s` into frame-persistent storage and returns a view valid until the next
    // begin_frame(). A deque, NOT a vector: appends must never relocate earlier strings.
    [[nodiscard]] std::string_view own(std::string s);
    [[nodiscard]] std::string_view own(std::string_view s) { return own(std::string(s)); }

    [[nodiscard]] const Theme& theme() const { return theme_; }
    [[nodiscard]] const interaction& interaction_state() const { return interaction_; }
    [[nodiscard]] Motion& motion() { return motion_; }
    [[nodiscard]] layout_builder& builder() { return builder_; }

private:
    friend class Element;

    layout_builder& builder_;
    const interaction& interaction_;
    Motion& motion_;
    const Theme& theme_;
    std::deque<std::string> arena_;
};

}  // namespace string::ui
