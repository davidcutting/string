#include <string/ui/ui.hpp>

namespace string::ui
{

// --- Ui ------------------------------------------------------------------------------------------

void Ui::begin_frame()
{
    // The arena is per-frame: every view handed out last frame dies here, which is exactly when the
    // layout tree that referenced them is rebuilt.
    arena_.clear();
    motion_.begin_frame(interaction_.dt);
}

void Ui::end_frame()
{
    motion_.end_frame();
}

std::string_view Ui::own(std::string s)
{
    // deque, NOT vector: push_back must never relocate earlier strings — SSO string data lives
    // inside the string object, so a regrowth moves it and dangles every view taken so far. This
    // is the single place that rule now lives.
    arena_.push_back(std::move(s));
    return arena_.back();
}

Element Ui::element(std::string_view name)
{
    return Element(*this, make_id(name));
}

Element Ui::panel(std::string_view name)
{
    Element e(*this, make_id(name));
    e.color(theme_.panel)
     .stroke(theme_.stroke, theme_.stroke_width)
     .radius(theme_.radius)
     .shape(shape::ROUNDED_RECTANGLE)
     .fit()
     .pad(theme_.pad)
     .gap(theme_.gap)
     .column();
    return e;
}

Element Ui::box()
{
    // id{} — NOT make_id(""), which would hash to a real (and shared!) value and make every
    // anonymous element collide.
    return Element(*this, id{});
}

Element Ui::text(std::string_view content)
{
    Element e(*this, id{});
    e.color(theme_.text).fit().label(content);
    return e;
}

Element Ui::label(std::string_view name, std::string_view text)
{
    Element e(*this, make_id(name));
    e.color(theme_.text).fit().label(text).font(theme_.font_px);
    return e;
}

Element Ui::button(std::string_view name)
{
    Element e(*this, make_id(name));
    const std::uint64_t h = e.id_hash();
    const bool hot = interaction_.is_hot(h) || interaction_.is_focused(h);
    const bool down = interaction_.is_pressed(h);

    // Hover/press feel comes from the THEME's transitions, not from per-widget constants.
    const string::color target = down ? theme_.accent : (hot ? theme_.stroke_hi : theme_.panel_alt);
    e.color(motion_.animate_color(h, target, theme_.hover))
     .stroke(interaction_.is_focused(h) ? theme_.accent_warm : theme_.stroke,
             interaction_.is_focused(h) ? 3 : theme_.stroke_width)
     .radius(8)
     .shape(shape::ROUNDED_RECTANGLE)
     .fit()
     .pad(8);
    return e;
}

// --- Element -------------------------------------------------------------------------------------

Element::Element(Ui& ui, id identity) : ui_(ui)
{
    element_.id = identity;
    // Defaults match a default-constructed `element`: transparent, shrink-wrap. Deliberately NOT
    // theme.text — a structural container that silently inherited a text colour would paint a
    // filled rectangle where the author asked for pure layout. Theme colour is applied by the
    // factories that actually mean it (text/label/panel/button).
    element_.sizing = size_fit();
    (void)ui;
}

Element::Element(Element&& other) noexcept
    : ui_(other.ui_), element_(other.element_), format_(other.format_), text_(other.text_),
      font_px_(other.font_px_), emitted_(other.emitted_)
{
    other.emitted_ = true;   // the source must never emit; exactly one of the pair does
}

Element::~Element()
{
    if (!emitted_)
        emit_leaf();
}

Element& Element::label(std::string_view text)
{
    // Copy into the frame arena: add_text takes a non-owning view, so a caller passing a temporary
    // (`u.label("n", "frame " + std::to_string(i))`) must not dangle.
    text_ = ui_.own(text);
    if (font_px_ == 0) font_px_ = ui_.theme().font_px;
    return *this;
}

Element& Element::font(std::uint16_t px) { font_px_ = px; return *this; }

Element& Element::color(string::color c) { element_.color = c; return *this; }

Element& Element::stroke(string::color c, std::uint16_t width)
{
    element_.stroke_color = c;
    element_.stroke_width = width;
    return *this;
}

Element& Element::radius(std::uint16_t r) { element_.radius = r; return *this; }
Element& Element::shape(string::shape s) { element_.shape = s; return *this; }

Element& Element::rounded()
{
    element_.radius = ui_.theme().radius;
    element_.shape = string::shape::ROUNDED_RECTANGLE;
    return *this;
}

Element& Element::sweep(std::uint8_t s) { element_.sweep = s; return *this; }

Element& Element::size(string::sizing s) { element_.sizing = s; return *this; }
Element& Element::width(axis_sizing a) { element_.sizing.width = a; return *this; }
Element& Element::height(axis_sizing a) { element_.sizing.height = a; return *this; }
Element& Element::grow() { element_.sizing = size_grow(); return *this; }
Element& Element::fit() { element_.sizing = size_fit(); return *this; }

Element& Element::fixed(std::uint16_t w, std::uint16_t h)
{
    element_.sizing = size_fixed(w, h);
    return *this;
}

Element& Element::row() { format_.direction = direction::HORIZONTAL; return *this; }
Element& Element::column() { format_.direction = direction::VERTICAL; return *this; }
Element& Element::gap(std::uint16_t g) { format_.gap = g; return *this; }
Element& Element::pad(std::uint16_t p) { format_.padding = { p, p, p, p }; return *this; }
Element& Element::pad(string::padding p) { format_.padding = p; return *this; }
Element& Element::align(alignment a) { format_.alignment = a; return *this; }
Element& Element::justify(justification j) { format_.justify = j; return *this; }

Element& Element::floating(std::uint16_t x, std::uint16_t y)
{
    element_.floating = true;
    element_.float_x = x;
    element_.float_y = y;
    return *this;
}

Element& Element::overlay() { element_.overlay = true; return *this; }

Element& Element::on_click(std::function<void()> fn)
{
    // Callback-first: fire immediately when this element is the one the resolved interaction says
    // was activated this frame. `clicked()` below reads the same state — poll IS sugar over this,
    // not a parallel mechanism.
    if (fn && ui_.interaction_state().is_pressed(element_.id.hash))
        fn();
    return *this;
}

bool Element::clicked() const { return ui_.interaction_state().is_pressed(element_.id.hash); }
bool Element::hovered() const { return ui_.interaction_state().is_hot(element_.id.hash); }
bool Element::focused() const { return ui_.interaction_state().is_focused(element_.id.hash); }
bool Element::dragging() const { return ui_.interaction_state().is_active(element_.id.hash); }

Element& Element::content(const std::function<void(Ui&)>& fn)
{
    // A container: open, author children, close. Text on a container is not meaningful (a text node
    // is a leaf), so it is ignored here rather than silently producing an unlaid-out run.
    emitted_ = true;
    ui_.builder_.begin(element_, format_);
    if (fn) fn(ui_);
    ui_.builder_.end();
    return *this;
}

void Element::emit_leaf()
{
    emitted_ = true;
    if (!text_.empty())
        ui_.builder_.add_text(element_, text_, font_px_);
    else
        ui_.builder_.add_element(element_);
}

}  // namespace string::ui
