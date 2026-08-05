#include <string/ui/text_edit.hpp>
#include <string/ui/widgets.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace string::ui
{
namespace
{
// Derive a widget_state key from a widget id plus a slot number, so each widget keeps its own set of
// persistent scalars without inventing names. Shared by table, tree, graph, scroll area and text
// field — it used to sit inside the table section, which meant the first widget above it that
// needed persistent state could not reach it.
[[nodiscard]] std::uint64_t slot(std::uint64_t base, std::uint64_t which) noexcept
{
    return base ^ (which * 0x9E3779B97F4A7C15ull);
}
}  // namespace

namespace
{

// Formats a value for display. snprintf rather than std::format: the engine targets a toolchain where
// <format> is not uniformly available, and the arena copy makes the temporary safe.
[[nodiscard]] std::string format_value(float v, int decimals)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", std::clamp(decimals, 0, 8), static_cast<double>(v));
    return buf;
}

}  // namespace

// --- Checkbox --------------------------------------------------------------------------------------

Checkbox::Checkbox(Ui& ui, std::string_view name, binding<bool> value)
    : ui_(ui), id_(make_id(ui.own(std::string(name)))), value_(std::move(value))
{
}

Checkbox::~Checkbox()
{
    if (!emitted_) emit();
}

Checkbox& Checkbox::label(std::string_view text)
{
    label_ = ui_.own(text);
    return *this;
}

Checkbox& Checkbox::on_change(std::function<void(bool)> fn)
{
    on_change_ = std::move(fn);
    return *this;
}

void Checkbox::emit()
{
    emitted_ = true;
    if (!value_.valid()) return;

    const Theme& th = ui_.theme();
    const interaction& ia = ui_.interaction_state();
    const bool on = value_.get();

    // The press is read BEFORE emitting so the visual reflects the new value on the same frame the
    // click happens — immediate mode's whole point, and what makes a toggle feel responsive.
    if (ia.is_pressed(id_.hash))
    {
        value_.set(!on);
        changed_ = true;
        if (on_change_) on_change_(!on);
    }
    const bool shown = changed_ ? !on : on;

    const bool hot = ia.is_hot(id_.hash) || ia.is_focused(id_.hash);
    Element row(ui_, id_);
    row.row().gap(th.gap).align(alignment::CENTER).fit().pad(padding{ 4, 4, 2, 2 }).focusable();
    row.content([&](Ui& u) {
        // The box. Filled when on, outlined when off — readable without relying on colour alone.
        u.element()
         .color(shown ? th.accent : th.panel_alt)
         .stroke(th.outline({ .hot = hot }), th.stroke_width)
         .radius(4)
         .shape(string::shape::ROUNDED_RECTANGLE)
         .fixed(16, 16);
        // The caption is ID-LESS on purpose: naming it would make the text the hover target instead
        // of the row, and the row is what you click.
        if (!label_.empty())
            u.text(label_).color(hot ? th.text : th.text_dim);
    });
}

// --- DragValue -------------------------------------------------------------------------------------

DragValue::DragValue(Ui& ui, std::string_view name, binding<float> value)
    : ui_(ui), id_(make_id(ui.own(std::string(name)))), value_(std::move(value))
{
}

DragValue::~DragValue()
{
    if (!emitted_) emit();
}

DragValue& DragValue::label(std::string_view text) { label_ = ui_.own(text); return *this; }
DragValue& DragValue::range(float lo, float hi) { lo_ = lo; hi_ = hi; return *this; }
DragValue& DragValue::step(float per_pixel) { step_ = per_pixel; return *this; }
DragValue& DragValue::precision(int decimals) { decimals_ = decimals; return *this; }

DragValue& DragValue::on_change(std::function<void(float)> fn)
{
    on_change_ = std::move(fn);
    return *this;
}

void DragValue::emit()
{
    emitted_ = true;
    if (!value_.valid()) return;

    const Theme& th = ui_.theme();
    const interaction& ia = ui_.interaction_state();
    float v = value_.get();

    // Value per pixel: derived from the range so a full sweep is ~200px unless the author says
    // otherwise. Without a derived default, every call site would have to invent a sensitivity.
    const float per_px = step_ > 0.0f ? step_ : (hi_ - lo_) / 200.0f;

    const bool active = ia.is_active(id_.hash);
    double& origin = ui_.widget_state(id_.hash, static_cast<double>(v));
    if (active)
    {
        if (!ia.dragging() || (ia.drag_x == 0.0f && ia.drag_y == 0.0f))
            origin = static_cast<double>(v);   // press frame: capture the value to measure from

        // origin + delta, NOT v += delta. The delta is absolute-from-press, so recomputing from a
        // captured origin is what keeps this from drifting when the value clamps at either end —
        // the same rule as panel drag and splitter drag.
        const float next = std::clamp(static_cast<float>(origin) + ia.drag_x * per_px, lo_, hi_);
        if (next != v)
        {
            value_.set(next);
            changed_ = true;
            if (on_change_) on_change_(next);
            v = next;
        }
    }

    const bool hot = ia.is_hot(id_.hash) || ia.is_focused(id_.hash);
    const float frac = (hi_ > lo_) ? std::clamp((v - lo_) / (hi_ - lo_), 0.0f, 1.0f) : 0.0f;

    Element row(ui_, id_);
    row.color(th.surface({ .hot = hot, .active = active }))
       .stroke(th.outline({ .hot = hot, .active = active }), th.stroke_width)
       .radius(6)
       .shape(string::shape::ROUNDED_RECTANGLE)
       .row()
       .gap(th.gap)
       .align(alignment::CENTER)
       .pad(padding{ 8, 8, 4, 4 })
       .fit()
       .focusable();
    row.content([&](Ui& u) {
        if (!label_.empty())
            u.text(label_).color(th.text_dim);

        // A FIXED-WIDTH track with a proportional fill and a handle.
        //
        // The track's width must not depend on the value. An earlier version sized a single bar to
        // `frac`, which — inside a `fit()` row — made the whole widget grow and shrink as you
        // dragged it. Constant track, moving fill: the value changes what is inside, never the
        // control's own footprint.
        constexpr std::uint16_t kTrack = 120;
        constexpr std::uint16_t kTrackH = 10;
        const auto fill_w = static_cast<std::uint16_t>(
            std::max<float>(kTrackH, frac * static_cast<float>(kTrack)));
        u.element()
         .color(th.panel_alt)
         .stroke(th.stroke, 1)
         .radius(kTrackH / 2)
         .shape(string::shape::ROUNDED_RECTANGLE)
         .fixed(kTrack, kTrackH)
         .row()
         .align(alignment::CENTER)
         .content([&](Ui& u2) {
             // One shape, same reasoning as Slider: the fill's own rounded end IS the handle.
             u2.element()
               .color(active ? th.accent_warm : (hot ? th.accent : th.stroke_hi))
               .radius(kTrackH / 2)
               .shape(string::shape::ROUNDED_RECTANGLE)
               .fixed(fill_w, kTrackH);
         });

        u.text(u.own(format_value(v, decimals_))).color(th.text);
    });
}

// --- Slider ----------------------------------------------------------------------------------------

Slider::Slider(Ui& ui, std::string_view name, binding<float> value)
    : ui_(ui), id_(make_id(ui.own(std::string(name)))), value_(std::move(value))
{
}

Slider::~Slider()
{
    if (!emitted_) emit();
}

Slider& Slider::label(std::string_view text) { label_ = ui_.own(text); return *this; }
Slider& Slider::range(float lo, float hi) { lo_ = lo; hi_ = hi; return *this; }
Slider& Slider::precision(int decimals) { decimals_ = decimals; return *this; }
Slider& Slider::width(std::uint16_t w) { width_ = w; return *this; }

Slider& Slider::on_change(std::function<void(float)> fn)
{
    on_change_ = std::move(fn);
    return *this;
}

void Slider::emit()
{
    emitted_ = true;
    if (!value_.valid()) return;

    const Theme& th = ui_.theme();
    const interaction& ia = ui_.interaction_state();
    float v = value_.get();

    constexpr std::uint16_t kTrackH = 12;

    // ABSOLUTE: the value is wherever the cursor is along the track. `hovered_box` is the track's
    // own rect, supplied by the hit test that already located it — so a press at 80% jumps to 80%
    // instead of nudging from wherever the value happened to be.
    const bool active = ia.is_active(id_.hash);
    if (active && ia.hovered_box.dimension.width > 0)
    {
        const float span = static_cast<float>(ia.hovered_box.dimension.width);
        const float rel = ia.cursor_x - static_cast<float>(ia.hovered_box.x);
        const float next = std::clamp(lo_ + (rel / span) * (hi_ - lo_), std::min(lo_, hi_),
                                      std::max(lo_, hi_));
        if (next != v)
        {
            value_.set(next);
            changed_ = true;
            if (on_change_) on_change_(next);
            v = next;
        }
    }

    const bool hot = ia.is_hot(id_.hash) || ia.is_focused(id_.hash);
    const float frac = (hi_ > lo_) ? std::clamp((v - lo_) / (hi_ - lo_), 0.0f, 1.0f) : 0.0f;
    // Floored at the track height so the fill is never a sliver: at zero it is a round dot sitting
    // at the left, which reads as the handle parked at the start.
    const auto fill_w =
        static_cast<std::uint16_t>(std::max<float>(kTrackH, frac * static_cast<float>(width_)));

    Element row(ui_, id{});
    row.row().gap(th.gap).align(alignment::CENTER).fit();
    row.content([&](Ui& u) {
        if (!label_.empty())
            u.text(label_).color(th.text_dim);

        // The TRACK carries the id: it is the thing being hit-tested, so `hovered_box` is its rect
        // and the cursor maths above is in its space. Giving the id to the row instead would make
        // the box include the label and the readout, and the value would map to the wrong span.
        Element track(u, id_);
        track.color(th.panel_alt)
             .stroke(th.outline({ .hot = hot, .active = active }), 1)
             .radius(kTrackH / 2)
             .shape(string::shape::ROUNDED_RECTANGLE)
             .fixed(width_, kTrackH)
             .row()
             .align(alignment::CENTER)
             .focusable();
        track.content([&](Ui& u2) {
            // ONE SHAPE, not two. A separate handle after the fill cannot work in flow layout: the
            // two cannot overlap (that needs root-space positioning we do not have at author time),
            // so you get either a rounded cap butting a circle — a visible seam — or a squared fill
            // clashing with the track's rounded left end. Giving the fill the track's OWN radius
            // makes its left end match exactly and its right end BE the handle.
            u2.element()
              .color(active ? th.accent_warm : (hot ? th.accent : th.stroke_hi))
              .radius(kTrackH / 2)
              .shape(string::shape::ROUNDED_RECTANGLE)
              .fixed(fill_w, kTrackH);
        });

        u.text(u.own(format_value(v, decimals_))).color(th.text);
    });
}

// --- TextField -------------------------------------------------------------------------------------

TextField::TextField(Ui& ui, std::string_view name, binding<std::string> value)
    : ui_(ui), id_(make_id(ui.own(std::string(name)))), value_(std::move(value))
{
}

TextField::~TextField()
{
    if (!emitted_) emit();
}

TextField& TextField::label(std::string_view text) { label_ = ui_.own(text); return *this; }
TextField& TextField::placeholder(std::string_view text) { placeholder_ = ui_.own(text); return *this; }
TextField& TextField::width(std::uint16_t w) { width_ = w; return *this; }
TextField& TextField::focus(bool forced) { force_focus_ = forced; return *this; }

TextField& TextField::on_change(std::function<void(const std::string&)> fn)
{
    on_change_ = std::move(fn);
    return *this;
}

TextField& TextField::on_submit(std::function<void(const std::string&)> fn)
{
    on_submit_ = std::move(fn);
    return *this;
}

void TextField::emit()
{
    emitted_ = true;
    if (!value_.valid()) return;

    const Theme& th = ui_.theme();
    const interaction& ia = ui_.interaction_state();
    std::string text = value_.get();
    const bool focused = force_focus_ || ia.is_focused(id_.hash);

    // Caret and selection survive the per-frame rebuild in `widget_state`, keyed off this field's id
    // — the same home a scroll offset and a drag origin use. Two slots, because a caret and an
    // anchor are two positions: a length could not say which end the user is dragging.
    constexpr std::uint64_t kCaretSlot = 20;
    constexpr std::uint64_t kAnchorSlot = 21;
    constexpr std::uint64_t kWasFocusedSlot = 22;
    double& caret_slot = ui_.widget_state(slot(id_.hash, kCaretSlot), 0.0);
    double& anchor_slot = ui_.widget_state(slot(id_.hash, kAnchorSlot), 0.0);
    double& was_focused = ui_.widget_state(slot(id_.hash, kWasFocusedSlot), 0.0);
    text_edit_state st{ static_cast<std::size_t>(caret_slot), static_cast<std::size_t>(anchor_slot) };
    st.clamp(text);   // the bound value can change under us; see text_edit.hpp

    // GAINING FOCUS PUTS THE CARET AT THE END. Without this a field opened on an existing value
    // starts with the caret at 0, so the first thing typed is PREPENDED and backspace does nothing —
    // both of which read as the field being broken rather than as a caret being somewhere else.
    if (focused && was_focused == 0.0)
    {
        st.caret = st.anchor = text.size();
        was_focused = 1.0;
    }
    else if (!focused)
    {
        was_focused = 0.0;
    }

    // ...AND SO DOES THE VALUE BEING REPLACED FROM OUTSIDE. The console's tab-completion and history
    // write the bound string directly, and a caret left at its old offset then sits in the middle of
    // the text that just appeared. `clamp` alone cannot see this: the string usually gets LONGER, so
    // the old offset stays valid while being wrong.
    //
    // Detected by hashing, because widget_state holds scalars and cannot keep a copy of the string.
    // A collision only means one missed snap, which degrades to the old behaviour rather than to
    // anything broken.
    constexpr std::uint64_t kSeenSlot = 23;
    double& seen = ui_.widget_state(slot(id_.hash, kSeenSlot), -1.0);
    const auto hash_of = [](std::string_view s) {
        return static_cast<double>(::string::fnv1a(s) >> 12);   // fits a double's mantissa exactly
    };
    if (seen >= 0.0 && seen != hash_of(text))
        st.caret = st.anchor = text.size();

    if (focused)
    {
        // Only the focused field consumes input. Requesting text capture is what stops the same
        // keystrokes ALSO driving the camera — the host owns capture, so this is a request, exactly
        // like the game/UI mode requests.
        ui_.request_text_capture();

        // All the editing rules live in `apply_text_edit`, which is pure logic over a string and is
        // unit-tested directly. This widget's job is to supply the keys and render the result.
        std::string copied;
        const text_edit_keys keys{
            ia.typed_text, ia.backspace, ia.del, ia.caret_left, ia.caret_right,
            ia.caret_home, ia.caret_end, ia.select_mod, ia.word_mod,
            ia.copy, ia.cut, ia.paste, ia.select_all, ia.clipboard,
        };
        if (apply_text_edit(text, st, keys, &copied))
        {
            changed_ = true;
            value_.set(text);
            if (on_change_) on_change_(text);
        }
        // The kit cannot touch a clipboard, so a copy is a REQUEST the host services — same shape as
        // text capture. Owned by the arena because the host reads it after authoring returns.
        if (!copied.empty()) ui_.request_clipboard_write(ui_.own(copied));

        // Submit is ROUTED, not polled: with a field inside a dialog inside a panel, exactly one of
        // them must act on Enter, and the router picked this one.
        if (ia.took(id_.hash, ui_action::submit) && on_submit_) on_submit_(text);
    }
    else
    {
        // Losing focus collapses the selection. A field showing a highlight it can no longer act on
        // reads as still-focused, which is exactly what focus is meant to communicate.
        st.anchor = st.caret;
    }

    caret_slot = static_cast<double>(st.caret);
    anchor_slot = static_cast<double>(st.anchor);
    // Recorded AFTER our own edits, so the next frame sees a match and only a write from OUTSIDE
    // this widget registers as the value having been replaced.
    seen = hash_of(text);

    const bool hot = ia.is_hot(id_.hash);
    const bool empty = text.empty();

    Element row(ui_, id{});
    row.row().gap(th.gap).align(alignment::CENTER).fit();
    row.content([&](Ui& u) {
        if (!label_.empty())
            u.text(label_).color(th.text_dim);

        Element box(u, id_);
        box.color(th.panel_alt)
           .stroke(th.outline({ .hot = hot, .focused = focused }), focused ? 2 : 1)
           .radius(4)
           .shape(string::shape::ROUNDED_RECTANGLE)
           .width(fixed(width_))
           .height(fit())
           .row()
           .gap(0)   // the text is split across several nodes; a gap would open holes mid-word
           .align(alignment::CENTER)
           .pad(padding{ 6, 6, 3, 3 })
           .focusable()   // the one widget that is USELESS without focus — it is how you type
           .handles(ui_action::submit);
        box.content([&](Ui& u2) {
            // Placeholder when empty, dimmed so it reads as a prompt rather than a value.
            if (empty && !placeholder_.empty())
            {
                u2.text(placeholder_).color(th.text_dim);
                if (focused) u2.element().color(th.accent).fixed(2, th.font_px);
                return;
            }

            // THE CARET AND SELECTION ARE DRAWN BY SPLITTING THE TEXT, NOT BY MEASURING IT.
            //
            // The kit owns no font atlas — measurement belongs to the host, which passes a measurer
            // to `builder.end()` — so a widget cannot ask how wide "hell" is in order to place a
            // caret after it. Emitting the run as up to four nodes lets LAYOUT do that positioning:
            // [before][selected][caret][after] in flow order, with the selected node carrying a fill.
            //
            // The cost is that kerning is not applied ACROSS a split boundary, so a caret sitting
            // between two kerned glyphs shifts the tail by a fraction of a pixel. That is invisible
            // at UI sizes and is the price of not dragging font metrics into a dependency-free kit.
            const std::size_t lo = st.selection_begin();
            const std::size_t hi = st.selection_end();
            const std::string_view all{ text };

            // Order matters: the caret goes where the CARET is, which may be either end of the
            // selection — dragging left must show the caret on the left.
            const auto emit_caret = [&] {
                if (focused) u2.element().color(th.accent).fixed(2, th.font_px);
            };
            const auto emit_run = [&](std::size_t from, std::size_t to) {
                if (to <= from) return;
                Element t(u2, id{});
                t.text(u2.own(std::string(all.substr(from, to - from)))).color(th.text);
            };

            if (!st.has_selection())
            {
                emit_run(0, st.caret);
                emit_caret();
                emit_run(st.caret, all.size());
            }
            else
            {
                emit_run(0, lo);
                if (st.caret == lo) emit_caret();
                // The selected run is its own node so it can carry a background fill. Text nodes
                // draw glyphs rather than a rect, so the highlight is a sibling behind it — see
                // below: an id-less container holding both.
                Element sel(u2, id{});
                sel.color(th.accent)
                   .radius(2)
                   .shape(string::shape::ROUNDED_RECTANGLE)
                   .row()
                   .gap(0)
                   .pad(0)
                   .fit();
                sel.content([&](Ui& u3) {
                    Element t(u3, id{});
                    t.text(u3.own(std::string(all.substr(lo, hi - lo)))).color(th.panel);
                });
                if (st.caret == hi) emit_caret();
                emit_run(hi, all.size());
            }
        });
    });
}

// --- Combo -----------------------------------------------------------------------------------------

Combo::Combo(Ui& ui, std::string_view name, binding<int> index)
    : ui_(ui), id_(make_id(ui.own(std::string(name)))), index_(std::move(index))
{
}

Combo::~Combo()
{
    if (!emitted_) emit();
}

Combo& Combo::label(std::string_view text) { label_ = ui_.own(text); return *this; }

Combo& Combo::options(std::span<const std::string_view> opts)
{
    options_ = opts;
    return *this;
}

Combo& Combo::on_change(std::function<void(int)> fn)
{
    on_change_ = std::move(fn);
    return *this;
}

void Combo::emit()
{
    emitted_ = true;
    if (!index_.valid()) return;

    const Theme& th = ui_.theme();
    const interaction& ia = ui_.interaction_state();
    const int current = index_.get();

    double& open = ui_.widget_state(id_.hash, 0.0);
    if (ia.is_pressed(id_.hash))
        open = (open > 0.5) ? 0.0 : 1.0;

    const bool hot = ia.is_hot(id_.hash) || ia.is_focused(id_.hash);
    const std::string_view shown =
        (current >= 0 && static_cast<std::size_t>(current) < options_.size())
            ? options_[static_cast<std::size_t>(current)]
            : std::string_view{ "—" };

    Element root(ui_, id{});
    root.column().gap(2).fit();
    root.content([&](Ui& u) {
        Element head(u, id_);
        head.color(th.surface({ .hot = hot }))
            .stroke(th.outline({ .hot = hot }), th.stroke_width)
            .radius(6)
            .shape(string::shape::ROUNDED_RECTANGLE)
            .row()
            .gap(th.gap)
            .align(alignment::CENTER)
            .pad(padding{ 8, 8, 4, 4 })
            .fit()
            .focusable();
        head.content([&](Ui& u2) {
            if (!label_.empty())
                u2.text(label_).color(th.text_dim);
            u2.text(shown).color(th.text);
            u2.text(open > 0.5 ? "v" : ">").color(th.text_dim).font(th.font_px_small);
        });

        if (open <= 0.5) return;

        // The options, in flow. Each needs its OWN stable id, derived from the combo's name and the
        // index — the same manual derivation `Panel` uses for its handles, and the reason identity
        // scoping is not yet required for this widget set.
        for (std::size_t i = 0; i < options_.size(); ++i)
        {
            Element opt(u, make_id(u.own(std::string(id_.name) + ".opt" + std::to_string(i))));
            const bool sel = static_cast<int>(i) == current;
            const bool opt_hot = u.interaction_state().is_hot(opt.id_hash());
            opt.color(th.surface({ .hot = opt_hot, .selected = sel }))
               .radius(4)
               .shape(string::shape::ROUNDED_RECTANGLE)
               .pad(padding{ 10, 10, 3, 3 })
               .width(grow())
               .fit()
               .focusable();
            opt.content([&](Ui& u2) { u2.text(options_[i]).color(sel ? th.text : th.text_dim); });
            if (opt.clicked())
            {
                index_.set(static_cast<int>(i));
                changed_ = true;
                open = 0.0;
                if (on_change_) on_change_(static_cast<int>(i));
            }
        }
    });
}

// --- Factories -------------------------------------------------------------------------------------

Checkbox checkbox(Ui& u, std::string_view name, binding<bool> value)
{
    return Checkbox(u, name, std::move(value));
}

DragValue drag_value(Ui& u, std::string_view name, binding<float> value)
{
    return DragValue(u, name, std::move(value));
}

Slider slider(Ui& u, std::string_view name, binding<float> value)
{
    return Slider(u, name, std::move(value));
}

TextField text_field(Ui& u, std::string_view name, binding<std::string> value)
{
    return TextField(u, name, std::move(value));
}

Combo combo(Ui& u, std::string_view name, binding<int> index)
{
    return Combo(u, name, std::move(index));
}

// --- Collapsible -----------------------------------------------------------------------------------

Collapsible::Collapsible(Ui& ui, std::string_view name)
    : ui_(ui), id_(make_id(ui.own(std::string(name))))
{
}

Collapsible& Collapsible::label(std::string_view text) { label_ = ui_.own(text); return *this; }
Collapsible& Collapsible::open(bool initially) { initial_ = initially; return *this; }

bool Collapsible::is_open() const
{
    return ui_.widget_state(id_.hash, initial_ ? 1.0 : 0.0) > 0.5;
}

bool Collapsible::content(const std::function<void(Ui&)>& fn)
{
    const Theme& th = ui_.theme();
    const interaction& ia = ui_.interaction_state();

    double& open_state = ui_.widget_state(id_.hash, initial_ ? 1.0 : 0.0);
    if (ia.is_pressed(id_.hash))
        open_state = (open_state > 0.5) ? 0.0 : 1.0;
    const bool open = open_state > 0.5;

    const bool hot = ia.is_hot(id_.hash) || ia.is_focused(id_.hash);

    Element outer(ui_, id{});
    outer.column().gap(4).width(grow()).height(fit());
    outer.content([&](Ui& u) {
        Element head(u, id_);
        head.color(th.surface({ .hot = hot }))
            .radius(4)
            .shape(string::shape::ROUNDED_RECTANGLE)
            .row()
            .gap(th.gap)
            .align(alignment::CENTER)
            .pad(padding{ 6, 6, 3, 3 })
            .width(grow())
            .height(fit())
            .focusable();
        head.content([&](Ui& u2) {
            // A chevron rather than a colour change alone: the open/closed state has to be readable
            // without relying on the theme having enough contrast between two surface tones.
            u2.text(open ? "v" : ">").color(th.text_dim).font(th.font_px_small);
            if (!label_.empty())
                u2.text(label_).color(hot ? th.text : th.text_dim);
        });

        // The body is NOT emitted when closed — the point of a collapsible in a debug UI is that a
        // folded section costs nothing, and emitting-then-hiding would cost everything but the draw.
        if (open && fn)
            u.element().column().gap(th.gap).pad(padding{ th.pad, 0, 0, 0 }).width(grow()).content(fn);
    });
    return open;
}

// --- Tabs ------------------------------------------------------------------------------------------

Tabs::Tabs(Ui& ui, std::string_view name, binding<int> index)
    : ui_(ui), id_(make_id(ui.own(std::string(name)))), index_(std::move(index))
{
}

Tabs::~Tabs()
{
    if (!emitted_) { emitted_ = true; emit_bar(); }
}

Tabs& Tabs::options(std::span<const std::string_view> labels) { labels_ = labels; return *this; }

Tabs& Tabs::on_change(std::function<void(int)> fn)
{
    on_change_ = std::move(fn);
    return *this;
}

void Tabs::emit_bar()
{
    if (!index_.valid()) return;

    const Theme& th = ui_.theme();
    const int current = index_.get();

    Element bar(ui_, id{});
    bar.row().gap(2).width(grow()).height(fit());
    bar.content([&](Ui& u) {
        for (std::size_t i = 0; i < labels_.size(); ++i)
        {
            // Derived per-tab ids, like the combo's options: each tab needs its own identity, and
            // deriving from the bar's name keeps that stable without the author naming every tab.
            Element tab(u, make_id(u.own(std::string(id_.name) + ".tab" + std::to_string(i))));
            const bool sel = static_cast<int>(i) == current;
            const bool hot = u.interaction_state().is_hot(tab.id_hash());
            tab.color(sel ? th.panel_alt : (hot ? th.panel : th.panel_alt))
               .stroke(sel ? th.accent : th.stroke, sel ? 2 : 1)
               .radius(4)
               .shape(string::shape::ROUNDED_RECTANGLE)
               .pad(padding{ 10, 10, 3, 3 })
               .fit()
               .focusable();
            tab.content([&](Ui& u2) {
                u2.text(labels_[i]).color(sel ? th.text : th.text_dim).font(th.font_px_small);
            });
            if (tab.clicked() && static_cast<int>(i) != current)
            {
                index_.set(static_cast<int>(i));
                if (on_change_) on_change_(static_cast<int>(i));
            }
        }
    });
}

Tabs& Tabs::content(const std::function<void(Ui&)>& fn)
{
    emitted_ = true;
    emit_bar();
    // The bar first, then the body: a click on the bar has already updated the index above, so the
    // body drawn below is the one just selected rather than the previous tab's.
    if (fn)
        ui_.element().column().gap(ui_.theme().gap).width(grow()).content(fn);
    return *this;
}

// --- ColorPicker -----------------------------------------------------------------------------------

ColorPicker::ColorPicker(Ui& ui, std::string_view name, binding<string::color> value)
    : ui_(ui), id_(make_id(ui.own(std::string(name)))), value_(std::move(value))
{
}

ColorPicker::~ColorPicker()
{
    if (!emitted_) emit();
}

ColorPicker& ColorPicker::label(std::string_view text) { label_ = ui_.own(text); return *this; }
ColorPicker& ColorPicker::alpha(bool show) { alpha_ = show; return *this; }

ColorPicker& ColorPicker::on_change(std::function<void(string::color)> fn)
{
    on_change_ = std::move(fn);
    return *this;
}

void ColorPicker::emit()
{
    emitted_ = true;
    if (!value_.valid()) return;

    const Theme& th = ui_.theme();
    const std::string base = std::string(id_.name);

    Element outer(ui_, id{});
    outer.column().gap(4).width(grow()).height(fit());
    outer.content([&](Ui& u) {
        u.element().row().gap(th.gap).align(alignment::CENTER).fit().content([&](Ui& u2) {
            // A SPLIT swatch: opaque hue on the left, the actual alpha over a pale backing on the
            // right. Showing only the opaque colour makes the alpha slider look broken; showing only
            // the blended one makes the hue unreadable exactly when alpha is low. The split gives
            // both, and it is what colour pickers conventionally do (usually over a checkerboard —
            // a flat backing is the same idea with the primitives we have).
            //
            // Children draw over their parent in tree order, so the alpha half really does blend
            // against the backing rather than being composited against the panel.
            //
            // SQUARE, not rounded: two halves meeting inside a rounded border reproduces exactly the
            // corner-wedge problem the slider had. Square swatches are conventional anyway.
            const string::color c = value_.get();
            constexpr std::uint16_t kSwatch = 30;
            constexpr std::uint16_t kHalf = kSwatch / 2;
            u2.element()
              .stroke(th.stroke, th.stroke_width)
              .fixed(kSwatch, kSwatch)
              .row()
              .gap(0)
              .pad(0)
              .content([&](Ui& u3) {
                  u3.element().color(string::color{ c.r, c.g, c.b, 255 }).fixed(kHalf, kSwatch);
                  u3.element()
                    .color(th.text)   // pale backing, so a low alpha reads as light rather than as the panel
                    .fixed(kHalf, kSwatch)
                    .content([&](Ui& u4) { u4.element().color(c).fixed(kHalf, kSwatch); });
              });
            if (!label_.empty())
                u2.text(label_).color(th.text_dim);
        });

        // One slider per channel, composed from the existing widget rather than reimplemented —
        // each gets a derived id, and the adapter binding converts between the 0-255 byte and the
        // float the slider speaks.
        const auto channel = [&](Ui& cu, std::string_view suffix, int which) {
            // std::string + std::string_view is P2591 (C++26) and MSVC's STL has not shipped it,
            // so materialise the view rather than relying on the operator.
            slider(cu, cu.own(base + std::string(suffix)),
                   binding<float>{
                       [this, which] {
                           const string::color c = value_.get();
                           const std::uint8_t v = which == 0 ? c.r : which == 1 ? c.g
                                                : which == 2 ? c.b : c.a;
                           return static_cast<float>(v);
                       },
                       [this, which](float f) {
                           string::color c = value_.get();
                           const auto v = static_cast<std::uint8_t>(std::clamp(f, 0.0f, 255.0f));
                           if (which == 0) c.r = v;
                           else if (which == 1) c.g = v;
                           else if (which == 2) c.b = v;
                           else c.a = v;
                           value_.set(c);
                           changed_ = true;
                           if (on_change_) on_change_(c);
                       } })
                .label(suffix.substr(1))
                .range(0.0f, 255.0f)
                .width(140)
                .precision(0);
        };
        channel(u, ".r", 0);
        channel(u, ".g", 1);
        channel(u, ".b", 2);
        if (alpha_) channel(u, ".a", 3);
    });
}

// --- M1 factories ----------------------------------------------------------------------------------

Collapsible collapsible(Ui& u, std::string_view name) { return Collapsible(u, name); }

Tabs tabs(Ui& u, std::string_view name, binding<int> index)
{
    return Tabs(u, name, std::move(index));
}

ColorPicker color_picker(Ui& u, std::string_view name, binding<string::color> value)
{
    return ColorPicker(u, name, std::move(value));
}

// --- Table -----------------------------------------------------------------------------------------

namespace
{
constexpr std::uint64_t kScrollSlot = 1;
constexpr std::uint64_t kSortColSlot = 2;
constexpr std::uint64_t kSortDirSlot = 3;
constexpr std::uint64_t kColWidthSlot = 100;   // + column index
constexpr std::uint64_t kColOriginSlot = 200;  // + column index; width at drag start
}  // namespace

Table::Table(Ui& ui, std::string_view name)
    : ui_(ui), id_(make_id(ui.own(std::string(name))))
{
}

Table::~Table()
{
    // Unlike the other widgets a table emits nothing on its own: without a row count and a renderer
    // there is no table, and silently drawing an empty frame would hide the missing call.
    (void)emitted_;
}

Table& Table::columns(std::span<const table_column> cols) { columns_ = cols; return *this; }
Table& Table::visible_rows(std::size_t n) { visible_ = std::max<std::size_t>(1, n); return *this; }
Table& Table::row_height(std::uint16_t px) { row_h_ = std::max<std::uint16_t>(8, px); return *this; }
Table& Table::selected(binding<int> index) { selected_ = std::move(index); return *this; }

Table& Table::on_sort(std::function<void(std::size_t, bool)> fn)
{
    on_sort_ = std::move(fn);
    return *this;
}

Table& Table::on_activate(std::function<void(std::size_t)> fn)
{
    on_activate_ = std::move(fn);
    return *this;
}

Table& Table::on_hover(std::function<void(std::size_t)> fn)
{
    on_hover_ = std::move(fn);
    return *this;
}

std::uint16_t Table::width_of(std::size_t col) const
{
    const std::uint16_t declared = col < columns_.size() ? columns_[col].width : 100;
    const double w = ui_.widget_state(slot(id_.hash, kColWidthSlot + col), declared);
    return static_cast<std::uint16_t>(std::clamp(w, 32.0, 2000.0));
}

std::size_t Table::first_visible() const
{
    return static_cast<std::size_t>(std::max(0.0, ui_.widget_state(slot(id_.hash, kScrollSlot), 0.0)));
}

Table& Table::rows(std::size_t count,
                   const std::function<void(Ui&, std::size_t, std::size_t)>& render)
{
    emitted_ = true;
    const Theme& th = ui_.theme();
    const interaction& ia = ui_.interaction_state();
    const std::string base = std::string(id_.name);

    const std::size_t max_first = count > visible_ ? count - visible_ : 0;
    double& scroll = ui_.widget_state(slot(id_.hash, kScrollSlot), 0.0);
    double& sort_col = ui_.widget_state(slot(id_.hash, kSortColSlot), -1.0);
    double& sort_dir = ui_.widget_state(slot(id_.hash, kSortDirSlot), 1.0);

    // --- Wheel and scrollbar drag, resolved before emitting so this frame shows the scrolled
    // position. The wheel moves whole ROWS here rather than pixels: this list is virtualized, so its
    // scroll position IS a row index and a sub-row offset would have nothing to apply itself to.
    const std::uint64_t bar_hash = make_id(ui_.own(base + ".scroll")).hash;
    const float notches = ia.wheel_for(id_.hash);
    if (notches != 0.0f) scroll -= static_cast<double>(notches) * 3.0;
    if (ia.is_active(bar_hash) && ia.hovered_box.dimension.height > 0 && max_first > 0)
    {
        const float rel = (ia.cursor_y - static_cast<float>(ia.hovered_box.y)) /
                          static_cast<float>(ia.hovered_box.dimension.height);
        scroll = std::clamp(static_cast<double>(rel) * static_cast<double>(max_first), 0.0,
                            static_cast<double>(max_first));
    }
    scroll = std::clamp(scroll, 0.0, static_cast<double>(max_first));
    const auto first = static_cast<std::size_t>(scroll);
    const std::size_t last = std::min(count, first + visible_);

    // --- Column resize, resolved BEFORE anything is emitted.
    //
    // The header cells are sized from these widths, so processing the drag inside the header loop
    // would apply it a frame late — the cell is built before the grip that follows it is read. Every
    // other widget here responds on the frame of the input, and a table that lags by one is the kind
    // of thing that feels broken without looking broken.
    std::vector<std::uint16_t> widths;
    widths.reserve(columns_.size());
    for (std::size_t c = 0; c < columns_.size(); ++c)
    {
        double& w = ui_.widget_state(slot(id_.hash, kColWidthSlot + c), columns_[c].width);
        double& w_origin = ui_.widget_state(slot(id_.hash, kColOriginSlot + c), w);
        const std::uint64_t grip = make_id(ui_.own(base + ".g" + std::to_string(c))).hash;
        if (ia.is_active(grip))
        {
            // Origin captured at press, then width = origin + delta. Same anti-drift rule as panel
            // drag, splitter drag and drag-value: the delta is absolute-from-press, so accumulating
            // it would bank every clamped pixel and desync from the cursor.
            if (ia.drag_x == 0.0f && ia.drag_y == 0.0f) w_origin = w;
            w = std::clamp(w_origin + static_cast<double>(ia.drag_x), 32.0, 2000.0);
        }
        widths.push_back(static_cast<std::uint16_t>(std::clamp(w, 32.0, 2000.0)));
    }

    Element outer(ui_, id_);
    outer.column().gap(0).fit().wheel();
    outer.content([&](Ui& u) {
        // --- Header. Clicking a column sorts by it; clicking the sorted column flips direction.
        u.element().row().gap(0).fit().content([&](Ui& u2) {
            for (std::size_t c = 0; c < columns_.size(); ++c)
            {
                Element head(u2, make_id(u2.own(base + ".h" + std::to_string(c))));
                const bool sorted = static_cast<int>(sort_col) == static_cast<int>(c);
                head.color(th.panel_alt)
                    .stroke(th.stroke, 1)
                    .width(fixed(widths[c]))
                    .height(fixed(row_h_))
                    .row()
                    .align(alignment::CENTER)
                    .pad(padding{ 6, 2, 0, 0 })
                    .focusable();   // clickable: sorts the column
                head.content([&](Ui& u3) {
                    u3.text(columns_[c].label).color(sorted ? th.text : th.text_dim)
                      .font(th.font_px_small);
                    if (sorted)
                        u3.text(sort_dir > 0 ? "^" : "v").color(th.accent).font(th.font_px_small);
                });
                if (head.clicked())
                {
                    if (sorted) sort_dir = -sort_dir;
                    else { sort_col = static_cast<double>(c); sort_dir = 1.0; }
                    if (on_sort_) on_sort_(c, sort_dir > 0);
                }

                // A resize grip on the column's trailing edge. Transparent until touched, for the
                // same reason the workspace splitter is: a filled divider between header cells that
                // already have strokes reads as clutter.
                const std::uint64_t grip = make_id(u2.own(base + ".g" + std::to_string(c))).hash;
                Element g(u2, id{ u2.own(base + ".g" + std::to_string(c)), grip });
                g.color(ia.is_active(grip) || ia.is_hot(grip) ? th.accent : color{ 0, 0, 0, 0 })
                 .width(fixed(4))
                 .height(fixed(row_h_));
            }
        });

        // --- Viewport + scrollbar.
        u.element().row().gap(0).fit().content([&](Ui& u2) {
            u2.element().column().gap(0).fit().content([&](Ui& u3) {
                for (std::size_t rrow = first; rrow < last; ++rrow)
                {
                    // Identity by ROW INDEX, so hover and selection stay attached to the same data
                    // row while scrolling rather than to the slot it happens to occupy.
                    Element row(u3, make_id(u3.own(base + ".r" + std::to_string(rrow))));
                    const bool sel = selected_.valid() && selected_.get() == static_cast<int>(rrow);
                    const bool hot = u3.interaction_state().is_hot(row.id_hash());
                    if (hot && on_hover_) on_hover_(rrow);
                    row.color(th.surface({ .hot = hot, .selected = sel }))
                       .row()
                       .gap(0)
                       .height(fixed(row_h_))
                       .fit()
                       .focusable();
                    row.content([&](Ui& u4) {
                        for (std::size_t c = 0; c < columns_.size(); ++c)
                        {
                            u4.element()
                              .width(fixed(static_cast<std::uint16_t>(widths[c] + 4)))
                              .height(fixed(row_h_))
                              .row()
                              .align(alignment::CENTER)
                              .pad(padding{ 6, 2, 0, 0 })
                              .content([&](Ui& u5) { if (render) render(u5, rrow, c); });
                        }
                    });
                    if (row.clicked())
                    {
                        if (selected_.valid()) selected_.set(static_cast<int>(rrow));
                        if (on_activate_) on_activate_(rrow);
                    }
                }
            });

            // The scrollbar track, only when there is something to scroll — a full-length thumb is a
            // control that cannot do anything. This bar sits IN FLOW (unlike the scroll area's,
            // which floats over a reserved gutter), so when it is absent an equally wide SPACER
            // takes its place: the table's width must not jump as rows are added or filtered away.
            const auto view_h = static_cast<std::uint16_t>(row_h_ * visible_);
            if (max_first == 0)
            {
                u2.element().fixed(10, view_h);
                return;
            }
            Element bar(u2, id{ u2.own(base + ".scroll"), bar_hash });
            bar.color(th.panel_alt).width(fixed(10)).height(fixed(view_h)).column().pad(1);
            bar.content([&](Ui& u3) {
                const double frac = count > 0 ? static_cast<double>(visible_) / static_cast<double>(count) : 1.0;
                const auto thumb_h = static_cast<std::uint16_t>(
                    std::clamp(frac * view_h, 12.0, static_cast<double>(view_h)));
                const double pos = max_first > 0 ? scroll / static_cast<double>(max_first) : 0.0;
                const auto pad_top = static_cast<std::uint16_t>(pos * (view_h - thumb_h));
                // A spacer above the thumb positions it: flow layout has no absolute placement
                // inside a parent, and the alternative (root-space `floating`) needs geometry we do
                // not have at author time.
                u3.element().fixed(1, pad_top);
                u3.element()
                  .color(ia.is_active(bar_hash) ? th.accent : th.stroke)
                  .radius(4)
                  .shape(string::shape::ROUNDED_RECTANGLE)
                  .fixed(8, thumb_h);
            });
        });
    });
    return *this;
}

Table table(Ui& u, std::string_view name) { return Table(u, name); }

// --- Tree ------------------------------------------------------------------------------------------

namespace
{
constexpr std::uint64_t kTreeScrollSlot = 4;
constexpr std::uint64_t kTreeExpandSalt = 0xC2B2AE3D27D4EB4Full;

// A hard ceiling on the flatten. `children` is the author's callback, so a mistake there — a node
// that reports itself as its own child — would otherwise hang the frame with no clue why. Bounded
// work and a visible truncation beats a lock-up.
constexpr std::size_t kMaxFlattened = 20000;
constexpr std::size_t kMaxDepth = 64;
}  // namespace

Tree::Tree(Ui& ui, std::string_view name) : ui_(ui), id_(make_id(ui.own(std::string(name)))) {}

Tree& Tree::visible_rows(std::size_t n) { visible_ = std::max<std::size_t>(1, n); return *this; }
Tree& Tree::row_height(std::uint16_t px) { row_h_ = std::max<std::uint16_t>(8, px); return *this; }
Tree& Tree::indent(std::uint16_t px) { indent_ = px; return *this; }
Tree& Tree::selected(binding<std::uint64_t> key) { selected_ = std::move(key); return *this; }
Tree& Tree::roots(std::span<const std::uint64_t> keys) { roots_ = keys; return *this; }

Tree& Tree::children(std::function<std::size_t(std::uint64_t)> count,
                     std::function<std::uint64_t(std::uint64_t, std::size_t)> child)
{
    count_ = std::move(count);
    child_ = std::move(child);
    return *this;
}

Tree& Tree::on_activate(std::function<void(std::uint64_t)> fn)
{
    on_activate_ = std::move(fn);
    return *this;
}

bool Tree::is_expanded(std::uint64_t key) const
{
    return ui_.widget_state(slot(id_.hash ^ (key * kTreeExpandSalt), 0), 0.0) > 0.5;
}

void Tree::set_expanded(std::uint64_t key, bool open)
{
    ui_.widget_state(slot(id_.hash ^ (key * kTreeExpandSalt), 0), 0.0) = open ? 1.0 : 0.0;
}

Tree& Tree::nodes(const std::function<void(Ui&, std::uint64_t, std::size_t)>& render)
{
    const Theme& th = ui_.theme();
    const interaction& ia = ui_.interaction_state();
    const std::string base = std::string(id_.name);

    // --- Flatten the EXPANDED set, depth first. A collapsed node contributes itself and nothing
    // below it, so a folded tree costs its roots no matter how large the data is.
    struct visible_node { std::uint64_t key; std::size_t depth; bool has_children; };
    std::vector<visible_node> flat;
    const auto build_flat = [&] {
        flat.clear();
        struct frame { std::uint64_t key; std::size_t depth; };
        std::vector<frame> stack;
        for (std::size_t i = roots_.size(); i-- > 0;) stack.push_back({ roots_[i], 0 });
        while (!stack.empty() && flat.size() < kMaxFlattened)
        {
            const frame f = stack.back();
            stack.pop_back();
            const std::size_t n = count_ ? count_(f.key) : 0;
            flat.push_back({ f.key, f.depth, n > 0 });
            if (n == 0 || !is_expanded(f.key) || f.depth + 1 >= kMaxDepth) continue;
            for (std::size_t i = n; i-- > 0;)
                stack.push_back({ child_ ? child_(f.key, i) : 0, f.depth + 1 });
        }
    };
    build_flat();

    // --- Same-frame expand/collapse. The flatten decides which rows exist, so a toggle handled
    // inside the row loop below would only take effect NEXT frame — the same lag the tables column
    // resize had. The press was resolved against last frames tree, so the node it names is in the
    // set we just built: find it, toggle, and rebuild before anything is emitted.
    if (ia.pressed != 0)
    {
        for (const visible_node& n : flat)
        {
            if (!n.has_children) continue;
            // Hashed from a temporary rather than the frame arena: this scan runs only on click
            // frames, and it would otherwise allocate one string per visible node for nothing.
            if (fnv1a(base + ".x" + std::to_string(n.key)) != ia.pressed) continue;
            set_expanded(n.key, !is_expanded(n.key));
            build_flat();
            break;
        }
    }

    const std::size_t count = flat.size();
    const std::size_t max_first = count > visible_ ? count - visible_ : 0;
    double& scroll = ui_.widget_state(slot(id_.hash, kTreeScrollSlot), 0.0);

    const std::uint64_t bar_hash = make_id(ui_.own(base + ".scroll")).hash;
    const float notches = ia.wheel_for(id_.hash);
    if (notches != 0.0f) scroll -= static_cast<double>(notches) * 3.0;   // whole rows, as in Table
    if (ia.is_active(bar_hash) && ia.hovered_box.dimension.height > 0 && max_first > 0)
    {
        const float rel = (ia.cursor_y - static_cast<float>(ia.hovered_box.y)) /
                          static_cast<float>(ia.hovered_box.dimension.height);
        scroll = std::clamp(static_cast<double>(rel) * static_cast<double>(max_first), 0.0,
                            static_cast<double>(max_first));
    }
    scroll = std::clamp(scroll, 0.0, static_cast<double>(max_first));
    const auto first = static_cast<std::size_t>(scroll);
    const std::size_t last = std::min(count, first + visible_);

    Element outer(ui_, id_);
    outer.row().gap(0).fit().wheel();
    outer.content([&](Ui& u) {
        u.element().column().gap(0).fit().content([&](Ui& u2) {
            for (std::size_t i = first; i < last; ++i)
            {
                const visible_node& n = flat[i];
                // Identity by the author's KEY, not by row position: expand state and selection must
                // follow the node when siblings above it open or close.
                Element row(u2, make_id(u2.own(base + ".n" + std::to_string(n.key))));
                const bool sel = selected_.valid() && selected_.get() == n.key;
                const bool hot = u2.interaction_state().is_hot(row.id_hash());
                row.color(th.surface({ .hot = hot, .selected = sel }))
                   .row()
                   .gap(4)
                   .align(alignment::CENTER)
                   .height(fixed(row_h_))
                   .pad(padding{ static_cast<std::uint16_t>(4 + n.depth * indent_), 6, 0, 0 })
                   .width(grow())
                   .focusable();
                row.content([&](Ui& u3) {
                    // The expander is its OWN target, separate from the row: opening a node and
                    // selecting it are different intents, and merging them makes it impossible to
                    // look inside a node without also selecting it.
                    if (n.has_children)
                    {
                        Element x(u3, make_id(u3.own(base + ".x" + std::to_string(n.key))));
                        const bool open = is_expanded(n.key);
                        x.fit().pad(padding{ 2, 2, 0, 0 });
                        x.content([&](Ui& u4) {
                            u4.text(open ? "v" : ">").color(th.text_dim).font(th.font_px_small);
                        });
                        // The toggle itself was applied before the flatten above; this element
                        // exists so the chevron is hittable and shows the resulting state.
                    }
                    else
                    {
                        // A spacer, so leaves line up with their expandable siblings rather than
                        // shifting left by the width of a chevron.
                        u3.element().fixed(10, 1);
                    }
                    if (render) render(u3, n.key, n.depth);
                });
                if (row.clicked())
                {
                    if (selected_.valid()) selected_.set(n.key);
                    if (on_activate_) on_activate_(n.key);
                }
            }
        });

        // Same rule as the table: no track when everything fits, but a spacer of the same width
        // holds the column so the tree does not reflow as branches open and close.
        const auto view_h = static_cast<std::uint16_t>(row_h_ * visible_);
        if (max_first == 0)
        {
            u.element().fixed(10, view_h);
            return;
        }
        Element bar(u, id{ u.own(base + ".scroll"), bar_hash });
        bar.color(th.panel_alt).width(fixed(10)).height(fixed(view_h)).column().pad(1);
        bar.content([&](Ui& u2) {
            const double frac =
                count > 0 ? static_cast<double>(visible_) / static_cast<double>(count) : 1.0;
            const auto thumb_h = static_cast<std::uint16_t>(
                std::clamp(frac * view_h, 12.0, static_cast<double>(view_h)));
            const double pos = max_first > 0 ? scroll / static_cast<double>(max_first) : 0.0;
            u2.element().fixed(1, static_cast<std::uint16_t>(pos * (view_h - thumb_h)));
            u2.element()
              .color(ia.is_active(bar_hash) ? th.accent : th.stroke)
              .radius(4)
              .shape(string::shape::ROUNDED_RECTANGLE)
              .fixed(8, thumb_h);
        });
    });
    return *this;
}

Tree tree(Ui& u, std::string_view name) { return Tree(u, name); }

// --- Graph -----------------------------------------------------------------------------------------

namespace
{
constexpr std::uint64_t kPanXSlot = 5;
constexpr std::uint64_t kPanYSlot = 6;
constexpr std::uint64_t kPanOrigXSlot = 7;
constexpr std::uint64_t kPanOrigYSlot = 8;
constexpr std::uint64_t kZoomSlot = 9;
constexpr std::uint64_t kNodeXSalt = 0x2545F4914F6CDD1Dull;
constexpr std::uint64_t kNodeYSalt = 0x9E3779B97F4A7C15ull;
}  // namespace

Graph::Graph(Ui& ui, std::string_view name) : ui_(ui), id_(make_id(ui.own(std::string(name)))) {}

Graph& Graph::size(std::uint16_t w, std::uint16_t h) { w_ = w; h_ = h; return *this; }
Graph& Graph::node_size(std::uint16_t w, std::uint16_t h) { node_w_ = w; node_h_ = h; return *this; }
Graph& Graph::spacing(std::uint16_t x, std::uint16_t y) { gap_x_ = x; gap_y_ = y; return *this; }
Graph& Graph::selected(binding<std::uint64_t> key) { selected_ = std::move(key); return *this; }
Graph& Graph::edges(std::span<const graph_edge> e) { edges_ = e; return *this; }

Graph& Graph::on_activate(std::function<void(std::uint64_t)> fn)
{
    on_activate_ = std::move(fn);
    return *this;
}

Graph& Graph::nodes(std::span<const std::uint64_t> keys,
                    const std::function<void(Ui&, std::uint64_t)>& render)
{
    const Theme& th = ui_.theme();
    const interaction& ia = ui_.interaction_state();
    const std::string base = std::string(id_.name);

    // --- Layered layout: a node's column is its longest path from any source.
    //
    // LONGEST path, not shortest: it is what keeps every edge pointing forward, so a node always
    // sits to the right of everything feeding it. Shortest-path layering lets an edge run backwards
    // whenever one input is deeper than another, which for a render-graph DAG reads as a cycle that
    // is not there.
    std::vector<std::size_t> depth(keys.size(), 0);
    const auto index_of = [&](std::uint64_t k) -> std::size_t {
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == k) return i;
        return keys.size();
    };
    // Relax repeatedly rather than topologically sort: bounded by the node count, and a cyclic input
    // (which a debug view over live data can absolutely produce) terminates instead of looping.
    for (std::size_t pass = 0; pass < keys.size(); ++pass)
    {
        bool changed = false;
        for (const graph_edge& e : edges_)
        {
            const std::size_t a = index_of(e.from);
            const std::size_t b = index_of(e.to);
            if (a >= keys.size() || b >= keys.size()) continue;
            // Clamped to n-1: in a DAG no path is longer than that, and the clamp is what makes a
            // CYCLIC input degrade to a sane picture. Without it each relaxation pass pushes the
            // cycle a column further right until every node is culled and the canvas renders blank.
            const std::size_t want = std::min(depth[a] + 1, keys.size() - 1);
            if (depth[b] < want) { depth[b] = want; changed = true; }
        }
        if (!changed) break;
    }

    // Order within each column, in the caller's node order so the arrangement is stable frame to
    // frame rather than depending on iteration order.
    std::vector<std::size_t> row(keys.size(), 0);
    {
        std::vector<std::size_t> next_row;
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            if (depth[i] >= next_row.size()) next_row.resize(depth[i] + 1, 0);
            row[i] = next_row[depth[i]]++;
        }
    }

    // --- Pan, resolved BEFORE emitting. Anything that changes what gets emitted has to be settled
    // first, or it lands a frame late — the lesson from the workspace splitter, the table's column
    // resize and the tree's expander, all of which had exactly this shape.
    const std::uint64_t canvas_hash = make_id(ui_.own(base + ".canvas")).hash;
    double& pan_x = ui_.widget_state(slot(id_.hash, kPanXSlot), 0.0);
    double& pan_y = ui_.widget_state(slot(id_.hash, kPanYSlot), 0.0);
    double& pan_ox = ui_.widget_state(slot(id_.hash, kPanOrigXSlot), 0.0);
    double& pan_oy = ui_.widget_state(slot(id_.hash, kPanOrigYSlot), 0.0);
    // Pan is a SCROLL OFFSET (>= 0) subtracted from node positions, not a free translation: bounded
    // to the content extent, because panning into empty space is not useful. It is measured in
    // ZOOMED pixels, so it is re-anchored when the zoom changes (below) rather than being stored
    // unscaled and multiplied — the clamp has to be against the extent you can actually see.
    std::size_t columns = 0;
    std::size_t rows_max = 0;
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        columns = std::max(columns, depth[i] + 1);
        rows_max = std::max(rows_max, row[i] + 1);
    }

    // --- Zoom, also resolved before emitting (it changes node sizes AND what is culled).
    //
    // The wheel over a canvas ZOOMS rather than scrolls, which is what every node editor does and
    // what makes a large graph navigable at all: pan alone leaves you hunting a 40-node DAG through a
    // 520px window. `wheel_for` means a graph nested inside a scroll area takes the wheel while the
    // cursor is over it and the list scrolls everywhere else, with no coordination between the two.
    //
    // Exponential in notches so each click is the same PROPORTIONAL step — a linear increment feels
    // fast when zoomed out and glacial when zoomed in.
    double& zoom = ui_.widget_state(slot(id_.hash, kZoomSlot), 1.0);
    const double zoom_before = zoom;
    const float notches = ia.wheel_for(canvas_hash);
    if (notches != 0.0f)
        zoom = std::clamp(zoom * std::exp(static_cast<double>(notches) * 0.12), 0.35, 3.0);

    // Everything the layout produces is scaled, so zooming is one multiply here rather than a
    // transform the emit code has to thread through. Text is NOT scaled: glyph size is the render
    // callback's business (the author picks the font), and silently rescaling it would fight them.
    const auto zi = [&](std::uint16_t v) {
        return static_cast<int>(std::lround(static_cast<double>(v) * zoom));
    };
    const int node_w = std::max(8, zi(node_w_));
    const int node_h = std::max(8, zi(node_h_));
    const int gap_x = zi(gap_x_);
    const int gap_y = zi(gap_y_);

    const double content_w = static_cast<double>(columns) * (node_w + gap_x);
    const double content_h = static_cast<double>(rows_max) * (node_h + gap_y);
    const double max_pan_x = std::max(0.0, content_w - w_ + 16.0);
    const double max_pan_y = std::max(0.0, content_h - h_ + 16.0);
    if (ia.is_active(canvas_hash))
    {
        if (ia.drag_x == 0.0f && ia.drag_y == 0.0f) { pan_ox = pan_x; pan_oy = pan_y; }
        pan_x = pan_ox - static_cast<double>(ia.drag_x);   // drag right reveals content to the LEFT
        pan_y = pan_oy - static_cast<double>(ia.drag_y);
    }
    // Zoom about the canvas CENTRE: the content point under the middle of the view stays under it,
    // so zooming out does not walk the graph off to one side. (Anchoring on the CURSOR is the other
    // convention and reads slightly better, but it needs the canvas's screen rect — which this widget
    // deliberately does not know, since parent-relative placement is what keeps it position-free.)
    if (zoom != zoom_before)
    {
        const double ratio = zoom / zoom_before;
        pan_x = (pan_x + w_ * 0.5) * ratio - w_ * 0.5;
        pan_y = (pan_y + h_ * 0.5) * ratio - h_ * 0.5;
    }

    // Still clamped to the content extent — panning into empty space is not useful — but the
    // clamp no longer has to keep positions non-negative, since the canvas clips and the layout can
    // represent a node sitting partly off its left or top edge.
    pan_x = std::clamp(pan_x, 0.0, max_pan_x);
    pan_y = std::clamp(pan_y, 0.0, max_pan_y);

    // --- Node positions: layout, plus any manual offset, plus the pan.
    struct placed { std::uint64_t key; int x; int y; bool visible; };
    std::vector<placed> nodes_out;
    nodes_out.reserve(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        const std::uint64_t k = keys[i];
        // A dragged node's offset persists by KEY, so it survives the rebuild and stays with the
        // node even if the caller reorders its list.
        double& off_x = ui_.widget_state(id_.hash ^ (k * kNodeXSalt), 0.0);
        double& off_y = ui_.widget_state(id_.hash ^ (k * kNodeYSalt), 0.0);
        const std::uint64_t nh = fnv1a(base + ".nd" + std::to_string(k));
        if (ia.is_active(nh))
        {
            double& ox = ui_.widget_state(id_.hash ^ (k * kNodeXSalt) ^ 1u, off_x);
            double& oy = ui_.widget_state(id_.hash ^ (k * kNodeYSalt) ^ 1u, off_y);
            if (ia.drag_x == 0.0f && ia.drag_y == 0.0f) { ox = off_x; oy = off_y; }
            // Divided by the zoom because the offset is stored in graph units and the cursor moves in
            // screen pixels; without this a node zoomed to 2x travels twice as far as the pointer.
            off_x = ox + static_cast<double>(ia.drag_x) / zoom;
            off_y = oy + static_cast<double>(ia.drag_y) / zoom;
        }

        // Manual offsets are stored in SCREEN pixels at the zoom they were dragged at, then scaled
        // with everything else — so a node stays where you put it relative to the graph, not
        // relative to the window.
        const int x = static_cast<int>(static_cast<double>(depth[i]) * (node_w + gap_x)
                                       - pan_x + off_x * zoom) + 8;
        const int y = static_cast<int>(static_cast<double>(row[i]) * (node_h + gap_y)
                                       - pan_y + off_y * zoom) + 8;
        // VIEWPORT CULL: only what is FULLY outside is dropped. Off-canvas nodes are the common
        // case once a graph is panned and building them would defeat the point, but a node
        // straddling the edge is now both representable (signed positions) and safe to draw
        // (the canvas clips), so it slides off instead of popping.
        const bool visible = x + node_w > 0 && y + node_h > 0 && x < w_ && y < h_;
        nodes_out.push_back({ k, x, y, visible });
    }

    const std::uint64_t sel = selected_.valid() ? selected_.get() : 0;

    // The canvas itself is the pan target, so it must be id'd and sit UNDER the nodes in paint order.
    Element canvas(ui_, id{ ui_.own(base + ".canvas"), canvas_hash });
    canvas.color(th.panel_alt)
          .stroke(th.stroke, 1)
          .fixed(w_, h_)
          .clip()   // content may now be CUT at the edge instead of vanishing whole nodes early
          .wheel()  // the wheel zooms this canvas rather than whatever encloses it
          .pad(0);
    canvas.content([&](Ui& u) {
        // --- Edges first, so nodes paint over them.
        for (const graph_edge& e : edges_)
        {
            const auto ai = index_of(e.from);
            const auto bi = index_of(e.to);
            if (ai >= keys.size() || bi >= keys.size()) continue;
            const placed& a = nodes_out[ai];
            const placed& b = nodes_out[bi];
            // Both endpoints off-canvas means the whole edge is elsewhere. A HALF-visible edge is
            // fine now: the canvas clips, so the line is cut at the border instead of escaping and
            // painting over whatever sits beside the graph.
            if (!a.visible && !b.visible) continue;

            const bool lit = sel != 0 && (sel == e.from || sel == e.to);
            const string::color ec = lit ? th.accent : th.stroke;
            const int ax = a.x + node_w;
            const int ay = a.y + node_h / 2;
            const int bx = b.x;
            const int by = b.y + node_h / 2;
            const int mid = (ax + bx) / 2;
            const int thick = std::max(1, static_cast<int>(std::lround(2.0 * zoom)));

            // Three axis-aligned segments: out of A, across, into B. Each is an ordinary rect,
            // which is the only line primitive available.
            //
            // Only the FAR edge is clamped. The near edge may sit left of or above the canvas now
            // that positions are signed and the canvas clips — an edge into an off-screen node is cut
            // at the border, where it used to be dropped entirely and leave a node with no visible
            // wiring.
            const auto seg = [&](int x, int y, int w, int hgt) {
                w = std::min(w, static_cast<int>(w_) - x);
                hgt = std::min(hgt, static_cast<int>(h_) - y);
                if (w <= 0 || hgt <= 0) return;
                u.element()
                 .color(ec)
                 .floating(x, y)
                 .local()
                 .fixed(static_cast<std::uint16_t>(std::min(w, 4000)),
                        static_cast<std::uint16_t>(std::min(hgt, 4000)));
            };
            seg(ax, ay, mid - ax, thick);
            seg(mid, std::min(ay, by), thick, std::abs(by - ay) + thick);
            seg(mid, by, bx - mid, thick);
        }

        // --- Nodes.
        for (const placed& p : nodes_out)
        {
            if (!p.visible) continue;
            Element n(u, make_id(u.own(base + ".nd" + std::to_string(p.key))));
            const bool is_sel = sel == p.key;
            const bool hot = u.interaction_state().is_hot(n.id_hash());
            n.color(is_sel ? th.accent : (hot ? th.panel : th.panel_alt))
             .stroke(is_sel ? th.accent_warm : th.stroke, is_sel ? 2 : 1)
             .radius(6)
             .shape(string::shape::ROUNDED_RECTANGLE)
             .floating(p.x, p.y)
             .local()
             .fixed(static_cast<std::uint16_t>(node_w), static_cast<std::uint16_t>(node_h))
             .row()
             .align(alignment::CENTER)
             .pad(padding{ 6, 6, 0, 0 })
             .focusable();   // the NODES are the targets; the canvas behind them is a pan surface
            n.content([&](Ui& u2) { if (render) render(u2, p.key); });
            if (n.clicked())
            {
                if (selected_.valid()) selected_.set(p.key);
                if (on_activate_) on_activate_(p.key);
            }
        }
    });
    return *this;
}

Graph graph(Ui& u, std::string_view name) { return Graph(u, name); }

// --- Scroll area -----------------------------------------------------------------------------------

namespace
{
constexpr std::uint64_t kOffsetSlot = 10;
}  // namespace

ScrollArea::ScrollArea(Ui& ui, std::string_view name)
    : ui_(ui), id_(make_id(ui.own(std::string(name))))
{
}

ScrollArea& ScrollArea::size(std::uint16_t w, std::uint16_t h) { w_ = w; h_ = h; return *this; }

ScrollArea& ScrollArea::speed(float px_per_notch)
{
    speed_ = std::max(1.0f, px_per_notch);
    return *this;
}

float ScrollArea::offset() const
{
    return static_cast<float>(ui_.widget_state(slot(id_.hash, kOffsetSlot), 0.0));
}

ScrollArea& ScrollArea::content(const std::function<void(Ui&)>& fn)
{
    const Theme& th = ui_.theme();
    const interaction& ia = ui_.interaction_state();
    const std::string base = std::string(id_.name);

    constexpr std::uint16_t kBarW = 10;
    const std::uint64_t inner_hash = make_id(ui_.own(base + ".c")).hash;
    const std::uint64_t bar_hash = make_id(ui_.own(base + ".scroll")).hash;

    // The scroll extent comes from LAST frame's measured content — the only number here the author
    // cannot supply, because it is what the layout exists to compute. Until the content has been laid
    // out once this reads zero, which means "nothing to scroll": correct for an empty area and
    // self-correcting one frame later for a full one.
    const auto content_h = static_cast<double>(ui_.observed_box(inner_hash).dimension.height);
    const double max_off = std::max(0.0, content_h - static_cast<double>(h_));

    double& off = ui_.widget_state(slot(id_.hash, kOffsetSlot), 0.0);

    // --- Wheel and thumb drag, resolved BEFORE emitting: the offset decides where the content is
    // placed, so reading the input afterwards would show the previous position for a frame.
    const float notches = ia.wheel_for(id_.hash);
    if (notches != 0.0f)
        off -= static_cast<double>(notches) * static_cast<double>(speed_);
    if (ia.is_active(bar_hash) && ia.hovered_box.dimension.height > 0 && max_off > 0.0)
    {
        // Absolute, like the slider and the table's bar: press at 80% down the track and the view
        // goes to 80%. The track's own box arrives via `hovered_box`, which is populated for the drag
        // OWNER while a drag is in flight — exactly the window in which it is needed.
        const float rel = (ia.cursor_y - static_cast<float>(ia.hovered_box.y)) /
                          static_cast<float>(ia.hovered_box.dimension.height);
        off = static_cast<double>(rel) * max_off;
    }
    off = std::clamp(off, 0.0, max_off);

    Element view(ui_, id_);
    view.color(th.panel)
        .stroke(th.stroke, 1)
        .fixed(w_, h_)
        .clip()    // the whole point: content is CUT at the viewport edge
        .wheel()   // ...and the wheel that moves it is routed here from wherever the cursor is
        .pad(0);
    view.content([&](Ui& u) {
        // The content sits at a NEGATIVE offset — the case signed layout coordinates were added for.
        // It is out of flow (`floating`) and parent-relative (`local`), so the area never needs to
        // know where on screen it landed. Its height is `fit`, which is what makes it measurable.
        Element inner(u, id{ u.own(base + ".c"), inner_hash });
        inner.column()
             .gap(th.gap)
             .pad(padding{ 4, 4, 4, 4 })
             .width(fixed(static_cast<std::uint16_t>(w_ > kBarW ? w_ - kBarW : w_)))
             .height(fit())
             .floating(0, -static_cast<int>(std::lround(off)))
             .local();
        inner.content(fn);

        // The track, at the right edge — emitted ONLY when there is something to scroll. A bar that
        // spans its whole track is a control that cannot do anything, and reads as broken rather
        // than as "you can see everything".
        //
        // The GUTTER stays reserved either way: `inner` is always sized to w_ - kBarW, so the
        // content does not reflow when the bar appears or goes. That was the reason the bar used to
        // be unconditional, and it still holds — it just never needed the bar to be VISIBLE, only
        // the space to be spoken for.
        if (max_off > 0.0)
        {
            Element bar(u, id{ u.own(base + ".scroll"), bar_hash });
            bar.color(th.panel_alt)
               .floating(static_cast<int>(w_) - static_cast<int>(kBarW), 0)
               .local()
               .fixed(kBarW, h_)
               .column()
               .pad(1);
            bar.content([&](Ui& u2) {
                const double frac = content_h > 0.0
                                        ? std::min(1.0, static_cast<double>(h_) / content_h) : 1.0;
                const auto thumb_h = static_cast<std::uint16_t>(
                    std::clamp(frac * h_, 12.0, static_cast<double>(h_)));
                const double pos = off / max_off;
                const auto pad_top = static_cast<std::uint16_t>(pos * (h_ - thumb_h));
                u2.element().fixed(1, pad_top);   // spacer: flow layout has no absolute placement
                u2.element()
                  .color(ia.is_active(bar_hash) ? th.accent : th.stroke)
                  .radius(4)
                  .shape(string::shape::ROUNDED_RECTANGLE)
                  .fixed(8, thumb_h);
            });
        }
    });
    return *this;
}

ScrollArea scroll_area(Ui& u, std::string_view name) { return ScrollArea(u, name); }

// --- Menu bar --------------------------------------------------------------------------------------

namespace
{
constexpr std::uint64_t kOpenSlot = 11;
constexpr std::uint16_t kMenuBarH = 26;
constexpr std::uint16_t kMenuItemH = 22;
constexpr std::uint16_t kMenuW = 210;
// Menus sit above EVERYTHING, panels included (which clamp to 254). A transient popup that can be
// buried is a popup you cannot use.
// Within the POPUP layer, above the dock drop preview (1) — preserving the order the old absolute
// z values (255 vs 253) happened to give.
constexpr std::uint8_t kMenuZ = 2;

std::string menu_button_name(const std::string& base, std::size_t i)
{
    return base + ".m" + std::to_string(i);
}
}  // namespace

Menu& Menu::item(std::string_view label, std::function<void()> on_click)
{
    const Theme& th = ui_.theme();
    const std::string name = base_ + ".m" + std::to_string(index_) + ".i" + std::to_string(row_++);
    Element row(ui_, make_id(ui_.own(name)));
    const bool hot = ui_.interaction_state().is_hot(row.id_hash());
    row.color(hot ? th.accent : th.panel_alt)
       .width(grow())
       .height(fixed(kMenuItemH))
       .row()
       .align(alignment::CENTER)
       .pad(padding{ 10, 10, 0, 0 })
       .focusable();
    row.content([&](Ui& u) { u.text(label).color(hot ? th.panel : th.text).font(th.font_px_small); });
    if (row.clicked())
    {
        chose_ = true;
        if (on_click) on_click();
    }
    return *this;
}

Menu& Menu::toggle(std::string_view label, binding<bool> value)
{
    const bool on = value.valid() && value.get();
    // The marker is part of the LABEL rather than a second column: menu rows are short, and a
    // separate check column would make every item in every menu pay for the widest one.
    return item(ui_.own((on ? "* " : "  ") + std::string(label)),
                [value = std::move(value), on] { if (value.valid()) value.set(!on); });
}

Menu& Menu::separator()
{
    const Theme& th = ui_.theme();
    ui_.element().color(th.stroke).width(grow()).height(fixed(1));
    return *this;
}

MenuBar::MenuBar(Ui& ui, std::string_view name)
    : ui_(ui), id_(make_id(ui.own(std::string(name))))
{
}

MenuBar::~MenuBar()
{
    if (!emitted_) emit();
}

MenuBar& MenuBar::menu(std::string_view label, std::function<void(Menu&)> items)
{
    menus_.push_back(Entry{ label, std::move(items) });
    return *this;
}

std::uint16_t MenuBar::height() const { return kMenuBarH; }

void MenuBar::emit()
{
    emitted_ = true;
    const Theme& th = ui_.theme();
    const interaction& ia = ui_.interaction_state();
    const std::string base = std::string(id_.name);

    double& open = ui_.widget_state(slot(id_.hash, kOpenSlot), -1.0);

    // --- Resolve which menu is open BEFORE emitting anything: the open index decides which items
    // exist this frame, so deciding it during emission would show the previous state for a frame —
    // the same rule the table's column resize and the tree's expander are built on.
    const auto was_open = static_cast<int>(open);
    bool pressed_ours = false;
    for (std::size_t i = 0; i < menus_.size(); ++i)
    {
        const std::uint64_t bh = make_id(ui_.own(menu_button_name(base, i))).hash;
        if (ia.is_pressed(bh))
        {
            // Clicking the open menu's own button closes it; any other opens that one.
            open = (was_open == static_cast<int>(i)) ? -1.0 : static_cast<double>(i);
            pressed_ours = true;
        }
        // Hover-to-switch, but only while something is already open — otherwise merely sweeping the
        // bar would spring menus at you.
        else if (was_open >= 0 && ia.is_hot(bh) && was_open != static_cast<int>(i))
        {
            open = static_cast<double>(i);
        }
    }

    // Click-outside dismisses. `pressed` names whatever was activated this frame; if that is not one
    // of our buttons and not one of the open menu's items, the click was elsewhere. A click on empty
    // UI space activates nothing and instead asks for game mode, which counts too.
    if (!pressed_ours && open >= 0.0 && (ia.pressed != 0 || ia.wants_game_mode))
    {
        bool ours = false;
        const auto idx = static_cast<std::size_t>(open);
        for (std::size_t r = 0; r < 64 && !ours; ++r)
        {
            const std::string n = base + ".m" + std::to_string(idx) + ".i" + std::to_string(r);
            if (ia.is_pressed(make_id(ui_.own(n)).hash)) ours = true;
        }
        if (!ours) open = -1.0;
    }
    const int open_index = static_cast<int>(open);

    // --- The bar. FLOATING at the top of the screen rather than flowed, so it does not depend on
    // being authored before everything else — it is screen chrome, and an author that adds it last
    // should still get it at the top.
    Element bar(ui_, id_);
    bar.color(th.panel_alt)
       .stroke(th.stroke, 1)
       .floating(0, 0)
       .layer(ui_layer::popups)
       .z(kMenuZ)
       .fixed(static_cast<std::uint16_t>(ia.screen.width > 0 ? ia.screen.width : 1920), kMenuBarH)
       .row()
       .align(alignment::CENTER)
       .gap(0)
       .pad(padding{ 6, 6, 0, 0 });
    bar.content([&](Ui& u) {
        for (std::size_t i = 0; i < menus_.size(); ++i)
        {
            Element btn(u, make_id(u.own(menu_button_name(base, i))));
            const bool active = open_index == static_cast<int>(i);
            const bool hot = u.interaction_state().is_hot(btn.id_hash());
            btn.color(active ? th.accent : (hot ? th.panel : color{ 0, 0, 0, 0 }))
               .height(fixed(kMenuBarH))
               .row()
               .align(alignment::CENTER)
               .pad(padding{ 10, 10, 0, 0 })
               .focusable();
            btn.content([&](Ui& u2) {
                u2.text(menus_[i].label)
                  .color(active ? th.panel : th.text)
                  .font(th.font_px_small);
            });
        }
    });

    // --- The open menu's popup, DEFERRED to the popup layer.
    //
    // It is declared here and emitted after the main tree has been laid out, so its anchor query is
    // a SAME-FRAME read. It used to call `observed_box` — last frame's box — and the comment argued
    // that was fine because the bar does not move. That held right up until a bar inside a moving
    // panel, and it was a stale answer being defended rather than a right one.
    //
    // Everything the body needs is captured BY VALUE: it runs after this MenuBar is destroyed. The
    // label and name views point into the frame arena, which outlives authoring; `items` is a
    // copyable std::function.
    if (open_index >= 0 && open_index < static_cast<int>(menus_.size()))
    {
        const std::uint64_t bh = make_id(ui_.own(menu_button_name(base, open_index))).hash;
        const std::string_view owned_base = ui_.own(base);
        const id popup_id = make_id(ui_.own(base + ".popup"));
        auto items = menus_[static_cast<std::size_t>(open_index)].items;
        const auto index = static_cast<std::size_t>(open_index);
        double& open_slot = open;

        ui_.defer(ui_layer::popups, [bh, owned_base, popup_id, items, index,
                                     &open_slot](Ui& p) {
            const Theme& pt = p.theme();
            const bounding_box anchor = p.resolved_box(bh);
            const int x = anchor.dimension.width > 0 ? anchor.x : 0;
            const int y = anchor.dimension.width > 0 ? anchor.y + anchor.dimension.height
                                                     : kMenuBarH;

            Element popup(p, popup_id);
            popup.color(pt.panel_alt)
                 .stroke(pt.stroke_hi, 1)
                 .radius(6)
                 .shape(string::shape::ROUNDED_RECTANGLE)
                 .floating(x, y)
                 .layer(ui_layer::popups)
                 .z(kMenuZ)
                 .width(fixed(kMenuW))
                 .height(fit())
                 .column()
                 .gap(1)
                 .pad(padding{ 0, 0, 4, 4 });
            bool chose = false;
            popup.content([&](Ui& u) {
                Menu m(u, std::string(owned_base), index, chose);
                if (items) items(m);
            });

            // Choosing an item dismisses the menu — but only from the NEXT frame, because an item's
            // callback fires during emission and closing first would mean never emitting it, and so
            // never running it. The menu is therefore drawn once more on the frame you click, with
            // the item lit: feedback that the click landed, not a lag.
            if (chose) open_slot = -1.0;
        });
    }
}

MenuBar menu_bar(Ui& u, std::string_view name) { return MenuBar(u, name); }

}  // namespace string::ui
