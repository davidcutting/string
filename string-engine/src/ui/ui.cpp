#include <string/ui/ui.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace string::ui
{
namespace
{

// The layout tree addresses in integer pixels while a panel rect is float (sub-pixel drag). Round
// once, here, so the two never disagree about where an edge is.
//
// SIGNED: this used to clamp at zero, which silently snapped a panel dragged past the left or top
// edge back to the border instead of letting it slide — `update_panel` deliberately allows a panel
// to sit partly off-screen (it keeps a strip reachable), so clamping here fought that.
[[nodiscard]] int px(float v) noexcept
{
    return static_cast<int>(std::clamp(std::lround(v), -32768L, 32767L));
}

}  // namespace

// --- Ui ------------------------------------------------------------------------------------------

void Ui::begin_frame()
{
    // The arena is per-frame: every view handed out last frame dies here, which is exactly when the
    // layout tree that referenced them is rebuilt.
    arena_.clear();
    text_capture_ = false;   // re-asserted each frame by whichever field is focused
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

Element Ui::element()
{
    // id{} — NOT make_id(""), which would hash to a real (and shared!) value and make every
    // unnamed element collide.
    return Element(*this, id{});
}

Element Ui::text(std::string_view content)
{
    // Pure sugar: the theme colour and font now come from Element::text() itself, so this is
    // exactly element().text(content) with fewer characters.
    Element e(*this, id{});
    e.text(content);
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

Panel Ui::panel(std::string_view name)
{
    return Panel(*this, name);
}

// --- Panel --------------------------------------------------------------------------------------

Panel::Panel(Ui& ui, std::string_view name) : ui_(ui)
{
    // Sub-ids are derived from the panel name so they are stable across frames without the author
    // naming them. The hash is what carries identity (fnv1a of the same text every frame); the name
    // view only has to survive this frame, for the layout dump — hence the arena.
    id_ = make_id(ui.own(std::string(name)));
    move_id_ = make_id(ui.own(std::string(name) + ".title"));
    resize_id_ = make_id(ui.own(std::string(name) + ".grip"));
    handles_.move = move_id_.hash;
    handles_.resize = resize_id_.hash;
}

Panel& Panel::title(std::string_view text)
{
    title_ = ui_.own(text);
    return *this;
}

Panel& Panel::initial(panel_rect r) { initial_ = r; return *this; }
Panel& Panel::limits(panel_limits l) { limits_ = l; return *this; }

panel_rect Panel::rect() const
{
    const panel_state* st = ui_.panels_.find(id_.hash);
    return st != nullptr ? st->rect : initial_;
}

bool Panel::moving() const { return ui_.interaction_state().is_active(handles_.move); }
bool Panel::resizing() const { return ui_.interaction_state().is_active(handles_.resize); }

Panel& Panel::content(const std::function<void(Ui&)>& fn)
{
    const interaction& ia = ui_.interaction_state();
    const Theme& th = ui_.theme();

    // Resolve the drag BEFORE authoring, so this frame's tree is built at this frame's rect.
    panel_state& st = ui_.panels_.panel(id_.hash, initial_);
    update_panel(st, ia, handles_, limits_, ia.screen);
    const panel_rect r = st.rect;

    // Front-to-back order -> draw/hit z. +1 because z == 0 means "unset" (and so batches with every
    // other overlay element); the frontmost panel gets the highest value. Clamped because z is a
    // uint8 — beyond 254 panels the excess share the top layer, which is a far better failure than
    // wrapping around to the bottom.
    const auto depth = ui_.panels_.depth_of(id_.hash);
    const auto z = static_cast<std::uint8_t>(std::min<std::size_t>(depth + 1, 254));

    const auto bar_h = static_cast<std::uint16_t>(th.font_px_title + th.pad);
    constexpr std::uint16_t grip_size = 18;

    Element outer(ui_, id_);
    outer.color(th.panel)
         .stroke(moving() || resizing() ? th.accent : th.stroke, th.stroke_width)
         .radius(th.radius)
         .shape(string::shape::ROUNDED_RECTANGLE)
         .fixed(px(r.width), px(r.height))
         .floating(px(r.x), px(r.y))
         .overlay()   // panels float above ordinary HUD content, and hit-test above it
         .z(z)
         .column()
         .gap(0)
         .pad(0);

    // Arm click-to-raise for the duration of this panel's authoring. Saved/restored rather than
    // just cleared so a panel authored inside another panel's content still behaves.
    const std::uint64_t prev_panel = ui_.authoring_panel_;
    const bool prev_pressed = ui_.panel_pressed_;
    ui_.authoring_panel_ = id_.hash;
    ui_.panel_pressed_ = ia.is_pressed(id_.hash) || ia.is_pressed(handles_.move) ||
                          ia.is_pressed(handles_.resize);

    outer.content([&](Ui& u) {
        // --- Title bar: the move handle. Its caption is id-less on purpose — give it a name and
        // the caption becomes the hover target and the bar stops being draggable where the text is.
        Element bar(u, move_id_);
        bar.color(u.motion().animate_color(
               handles_.move,
               ia.is_active(handles_.move) ? th.accent
                                           : (ia.is_hot(handles_.move) ? th.stroke_hi : th.panel_alt),
               th.hover))
           .radius(th.radius)
           .shape(string::shape::ROUNDED_RECTANGLE)
           .width(grow())
           .height(fixed(bar_h))
           .row()
           .align(alignment::CENTER)
           .pad(padding{ th.pad, th.pad, 0, 0 });
        bar.content([&](Ui& u2) {
            if (!title_.empty())
                u2.text(title_).font(th.font_px_title);
        });

        // --- Body: ordinary flow, the author's content.
        u.element().grow().column().gap(th.gap).pad(th.pad).content(fn);
    });

    // --- Resize grip. A floating child, so it sits over the panel's bottom-right corner without
    // taking part in the column flow. `floating` is ROOT space (see layout.hpp), and we know the
    // panel's root-space rect, so the corner is computed directly rather than inferred post-layout.
    Element grip(ui_, resize_id_);
    grip.color(ia.is_active(handles_.resize) ? th.accent
                                             : (ia.is_hot(handles_.resize) ? th.stroke_hi : th.stroke))
        .radius(4)
        .shape(string::shape::ROUNDED_RECTANGLE)
        .fixed(grip_size, grip_size)
        .floating(px(r.right() - grip_size - 2.0f), px(r.bottom() - grip_size - 2.0f))
        .overlay()
        .z(z);

    // Raise AFTER authoring: the raise takes effect next frame, which is correct — reordering
    // mid-frame would mean this frame's tree disagreed with the z values already emitted into it.
    if (ui_.panel_pressed_)
        ui_.panels_.bring_to_front(id_.hash);
    ui_.authoring_panel_ = prev_panel;
    ui_.panel_pressed_ = prev_pressed;

    return *this;
}

// --- Element -------------------------------------------------------------------------------------

Element::Element(Ui& ui, id identity) : ui_(ui)
{
    element_.id = identity;
    // Click-to-raise: note if the element being authored is the one pressed this frame, so the
    // enclosing Panel (if any) can raise itself. See the note on Ui::authoring_panel_.
    if (ui.authoring_panel_ != 0 && identity.hash != 0 && identity.hash == ui.interaction_.pressed)
        ui.panel_pressed_ = true;
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

Element& Element::text(std::string_view content)
{
    // Copy into the frame arena: add_text takes a non-owning view, so a caller passing a temporary
    // (`u.text("frame " + std::to_string(i))`) must not dangle.
    text_ = ui_.own(content);
    if (font_px_ == 0) font_px_ = ui_.theme().font_px;
    // Theme the glyphs unless the author already chose a colour. Checking against the ctor default
    // needs no extra flag and works in either order: .color() before this is respected, .color()
    // after it overrides. See the header for why this belongs here and not in the constructor.
    if (element_.color.a == 0 && element_.color.r == 0 && element_.color.g == 0 &&
        element_.color.b == 0)
        element_.color = ui_.theme().text;
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

Element& Element::themed()
{
    const Theme& th = ui_.theme();
    // Deliberately does NOT set direction or sizing: VERTICAL and fit are already the defaults for
    // a fresh element, so setting them here would only be noise that hides the author's intent.
    element_.color = th.panel;
    element_.stroke_color = th.stroke;
    element_.stroke_width = th.stroke_width;
    element_.radius = th.radius;
    element_.shape = string::shape::ROUNDED_RECTANGLE;
    format_.padding = { th.pad, th.pad, th.pad, th.pad };
    format_.gap = th.gap;
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

Element& Element::floating(int x, int y)
{
    element_.floating = true;
    element_.float_x = detail::to_i16(x);
    element_.float_y = detail::to_i16(y);
    return *this;
}

Element& Element::overlay() { element_.overlay = true; return *this; }
Element& Element::local() { element_.float_local = true; return *this; }
Element& Element::z(std::uint8_t level) { element_.z = level; return *this; }
Element& Element::clip() { element_.clip = true; return *this; }
Element& Element::wheel() { element_.wheel = true; return *this; }
Element& Element::wrap() { element_.wrap = true; return *this; }

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

// --- Card ----------------------------------------------------------------------------------------

Card::Card(Ui& ui, std::uint64_t hash) : ui_(ui), hash_(hash) {}

Card& Card::title(std::string_view text)
{
    title_ = ui_.own(text);
    return *this;
}

Card& Card::content(std::function<void(Ui&)> fn)
{
    // A card outside a workspace has nowhere to be placed, so declaring one is meaningless rather
    // than an error worth aborting for — it simply does not draw.
    if (ui_.declaring_cards_ && hash_ != 0)
        ui_.cards_.push_back(Ui::CardDecl{ hash_, title_, std::move(fn) });
    return *this;
}

const Ui::CardDecl* Ui::find_card(std::uint64_t hash) const
{
    for (const CardDecl& c : cards_)
        if (c.hash == hash) return &c;
    return nullptr;
}

bool Ui::card_declared(std::uint64_t hash) const
{
    const CardDecl* d = find_card(hash);
    return d != nullptr && static_cast<bool>(d->body);
}

std::string_view Ui::card_title(std::uint64_t hash) const
{
    const CardDecl* d = find_card(hash);
    return d != nullptr ? d->title : std::string_view{};
}

void Ui::emit_card(std::uint64_t hash)
{
    const CardDecl* d = find_card(hash);
    if (d != nullptr && d->body) d->body(*this);
}

Card Ui::card(std::uint64_t hash) { return Card(*this, hash); }

double& Ui::widget_state(std::uint64_t id, double initial)
{
    const auto it = widget_state_.find(id);
    if (it != widget_state_.end()) return it->second;
    return widget_state_.emplace(id, initial).first->second;
}

bounding_box Ui::observed_box(std::uint64_t id)
{
    // Registering on READ is what keeps this opt-in without a separate register call: an id nobody
    // asks about is never cached, and the first ask costs one empty box.
    const auto it = observed_.find(id);
    if (it != observed_.end()) return it->second;
    observed_.emplace(id, bounding_box{});
    return bounding_box{};
}

void Ui::observe()
{
    if (observed_.empty()) return;
    for (const layout_node& n : builder_.nodes())
    {
        if (n.element.id.hash == 0) continue;   // id-less nodes are not addressable
        const auto it = observed_.find(n.element.id.hash);
        if (it != observed_.end()) it->second = n.box;
    }
}

WorkspaceView Ui::workspace(Workspace& ws) { return WorkspaceView(*this, ws); }

// --- Workspace emit ------------------------------------------------------------------------------
//
// THE INVARIANT: this walk EMITS, it does not compute geometry. Every size below is handed to the
// layout engine as an `axis_sizing`; nothing here calculates a rect.
namespace
{

// A node sizes along its parent split's axis and grows on the cross axis.
//
// The minimum is refreshed from the SUBTREE every frame rather than trusted from the stored sizing:
// a node's real floor changes as cards are docked and closed beneath it, and a stale one lets the
// layout squeeze a nested split until its children overflow and overlap the neighbouring region.
void apply_sizing(Element& e, Workspace& ws, node_index index, direction parent_dir)
{
    axis_sizing s = ws.node(index).sizing;
    s.min = std::max(s.min, ws.effective_min(index, parent_dir));
    if (parent_dir == direction::HORIZONTAL)
        e.width(s).height(grow());
    else
        e.height(s).width(grow());
}

void emit_node(Ui& u, Workspace& ws, node_index index, direction parent_dir);

void emit_panel(Ui& u, Workspace& ws, node_index index, direction parent_dir)
{
    workspace_node& n = ws.node(index);
    const Theme& th = u.theme();
    const std::uint64_t uid = n.uid;
    const std::string_view name = u.own("ws.panel." + std::to_string(uid));
    n.element_hash = make_id(name).hash;

    Element e = u.element(name);
    apply_sizing(e, ws, index, parent_dir);
    e.color(th.panel)
     .stroke(th.stroke, th.stroke_width)
     .radius(th.radius)
     .shape(string::shape::ROUNDED_RECTANGLE)
     .column()
     .gap(0)
     .pad(0);

    // Copied: a tab click calls select() mid-walk, which would otherwise mutate what we iterate.
    const std::vector<std::uint64_t> cards = n.cards;
    const std::size_t selected = cards.empty() ? 0 : std::min(n.selected, cards.size() - 1);

    e.content([&](Ui& u2) {
        // --- Tab bar. Present even for a single card: it is the panel's title, and in M2d it becomes
        // the tear-off grab handle, so it must not appear and vanish with the tab count.
        u2.element(u2.own("ws.tabs." + std::to_string(uid)))
          .width(grow())
          .height(fixed(static_cast<std::uint16_t>(th.font_px + th.pad)))
          .row()
          .gap(2)
          .pad(padding{ 4, 4, 0, 0 })
          .align(alignment::CENTER)
          .content([&](Ui& u3) {
              for (std::size_t i = 0; i < cards.size(); ++i)
              {
                  const std::string_view t = u3.card_title(cards[i]);
                  const std::string_view label = t.empty() ? "card" : t;
                  const bool active = i == selected;
                  Element tab = u3.element(u3.own("ws.tab." + std::to_string(uid) + "." +
                                                  std::to_string(cards[i])));
                  tab.color(active ? th.panel_alt : th.panel)
                     .radius(6)
                     .shape(string::shape::ROUNDED_RECTANGLE)
                     .pad(padding{ 8, 8, 2, 2 })
                     .fit();
                  tab.content([&](Ui& u4) {
                      u4.text(label).color(active ? th.text : th.text_dim).font(th.font_px_small);
                  });
                  if (tab.clicked())
                      ws.select(cards[i]);

                  // --- Pick the card up. A tab is a drag handle as well as a button: hold and move
                  // beyond a threshold and the card is being carried. The threshold is what keeps a
                  // click from becoming a drag — without it every tab press would tear the card out.
                  const std::uint64_t tab_hash = tab.id_hash();
                  const interaction& tia = u3.interaction_state();
                  if (tia.is_active(tab_hash) && !ws.dragging_card().active)
                  {
                      constexpr float kPickUp = 18.0f;
                      if (std::abs(tia.drag_x) + std::abs(tia.drag_y) > kPickUp)
                          ws.begin_card_drag(cards[i], tab_hash);
                  }
              }
          });

        // --- The selected card's body. A card the arrangement references but which was not declared
        // this frame is SHOWN as missing rather than skipped: a silently blank panel is hard to
        // diagnose, and this is exactly what a typo'd card id looks like.
        u2.element().grow().column().gap(th.gap).pad(th.pad).content([&](Ui& u3) {
            if (cards.empty()) return;
            if (!u3.card_declared(cards[selected]))
                u3.text("(not declared this frame)").color(th.text_dim).font(th.font_px_small);
            else
                u3.emit_card(cards[selected]);
        });
    });
}

void emit_split(Ui& u, Workspace& ws, node_index index, direction parent_dir)
{
    workspace_node& n = ws.node(index);
    const Theme& th = u.theme();
    const interaction& ia = u.interaction_state();
    const std::uint64_t uid = n.uid;
    const std::string_view name = u.own("ws.split." + std::to_string(uid));
    n.element_hash = make_id(name).hash;

    const bool horizontal = n.dir == direction::HORIZONTAL;
    const std::string_view grip_name = u.own("ws.grip." + std::to_string(uid));
    const std::uint64_t grip_hash = make_id(grip_name).hash;

    // --- Splitter drag. The origin is captured on the frame the drag begins, from sizes `observe()`
    // recorded after LAST frame's layout — the only way to turn a pixel delta into a sizing change,
    // since a container's size does not exist at authoring time. Recomputing from the captured origin
    // rather than accumulating is what keeps it drift-free, exactly as `update_panel` does.
    const bool active = !n.locked && ia.is_active(grip_hash);
    if (active && !n.dragging)
    {
        n.dragging = true;
        n.drag_a = ws.node(n.a).resolved;
        n.drag_b = ws.node(n.b).resolved;
    }
    else if (!active)
    {
        n.dragging = false;
    }
    if (active)
        ws.drag_splitter(index, horizontal ? ia.drag_x : ia.drag_y, n.drag_a, n.drag_b);

    // Read out before authoring: the closure below may reallocate the node vector via select().
    const node_index a = n.a;
    const node_index b = n.b;
    const direction dir = n.dir;
    const bool locked = n.locked;

    Element e = u.element(name);
    apply_sizing(e, ws, index, parent_dir);
    if (horizontal) e.row(); else e.column();
    e.gap(0).pad(0);
    e.content([&](Ui& u2) {
        emit_node(u2, ws, a, dir);

        // The splitter is an ordinary id'd element, so it hovers, focuses and captures the pointer
        // through the same path as any button. A LOCKED one is id-less — not an interaction target,
        // which is the same "id-lessness means decoration" rule the rest of the facade uses.
        //
        // THE BRACES ARE LOAD-BEARING: an Element emits in its DESTRUCTOR, so a named local would
        // emit at the END of this lambda — after child `b` — and the splitter would lay out past
        // both regions instead of between them. Scoping it forces emission here, in flow order.
        {
            constexpr std::uint16_t kThickness = 6;
            Element s = locked ? u2.element() : u2.element(grip_name);
            // TRANSPARENT until touched. A filled bar between two ROUNDED panels reads badly: the
            // corners curve away from it and leave wedges at each end. Left empty, the same 6px is
            // simply the gutter between two panels, and the hover tint is the only affordance —
            // which is also what a locked splitter looks like permanently, correctly, since it is
            // id-less and can never highlight.
            const bool touched = !locked && (ia.is_active(grip_hash) || ia.is_hot(grip_hash));
            s.color(touched ? th.accent : color{ 0, 0, 0, 0 })
             .radius(kThickness / 2)
             .shape(string::shape::ROUNDED_RECTANGLE);
            if (horizontal)
                s.width(fixed(kThickness)).height(grow());
            else
                s.height(fixed(kThickness)).width(grow());
        }

        emit_node(u2, ws, b, dir);
    });
}

void emit_node(Ui& u, Workspace& ws, node_index index, direction parent_dir)
{
    if (!ws.valid(index)) return;
    if (ws.node(index).is_split())
        emit_split(u, ws, index, parent_dir);
    else
        emit_panel(u, ws, index, parent_dir);
}

// A floating panel: the SAME panel object as a docked one, wrapped in movable/resizable chrome.
// Position and drag bookkeeping go through M1's `update_panel`, so floating here behaves identically
// to a standalone `u.panel(...)` — floating is not a second mechanism.
void emit_floating(Ui& u, Workspace& ws, node_index index, std::uint8_t z)
{
    workspace_node& n = ws.node(index);
    const Theme& th = u.theme();
    const interaction& ia = u.interaction_state();
    const std::uint64_t uid = n.uid;

    const std::string_view move_name = u.own("ws.float." + std::to_string(uid));
    const std::string_view grip_name = u.own("ws.floatgrip." + std::to_string(uid));
    panel_handles handles{ make_id(move_name).hash, make_id(grip_name).hash };

    panel_limits limits{ 180.0f, static_cast<float>(ws.min_region()), 48.0f };
    update_panel(n.floating_state, ia, handles, limits, ia.screen);
    const panel_rect r = n.floating_state.rect;

    Element outer(u, make_id(u.own("ws.floatpanel." + std::to_string(uid))));
    outer.color(th.panel)
         .stroke(th.stroke, th.stroke_width)
         .radius(th.radius)
         .shape(string::shape::ROUNDED_RECTANGLE)
         .fixed(px(r.width), px(r.height))
         .floating(px(r.x), px(r.y))
         .overlay()
         .z(z)
         .column()
         .gap(0)
         .pad(0);

    outer.content([&](Ui& u2) {
        // A move bar above the tab bar, so tabs stay draggable AS TABS (to re-dock) while the panel
        // itself is still movable. Overloading the tab bar for both would make the two gestures
        // fight over the same pixels.
        u2.element(move_name)
          .color(ia.is_active(handles.move) ? th.accent : th.panel_alt)
          .radius(th.radius)
          .shape(string::shape::ROUNDED_RECTANGLE)
          .width(grow())
          .height(fixed(20));
        emit_panel(u2, ws, index, direction::VERTICAL);
    });

    Element grip(u, make_id(grip_name));
    grip.color(ia.is_active(handles.resize) ? th.accent : th.stroke)
        .radius(4)
        .shape(string::shape::ROUNDED_RECTANGLE)
        .fixed(16, 16)
        .floating(px(r.right() - 18.0f), px(r.bottom() - 18.0f))
        .overlay()
        .z(z);
}

// The drop preview. Derived HERE, in the emit layer, from the box `observe()` cached — deliberately
// not on `Workspace`, which must not grow a function that returns a rect.
void emit_drop_preview(Ui& u, Workspace& ws)
{
    const Workspace::card_drag& d = ws.dragging_card();
    if (!d.active || d.target == no_node || !ws.valid(d.target)) return;

    const panel_rect b = ws.node(d.target).box;
    if (b.width <= 0.0f || b.height <= 0.0f) return;

    panel_rect p = b;
    if (!d.as_tab)
    {
        switch (d.zone)
        {
            case side::Left:   p.width *= 0.5f; break;
            case side::Right:  p.x += b.width * 0.5f; p.width *= 0.5f; break;
            case side::Top:    p.height *= 0.5f; break;
            case side::Bottom: p.y += b.height * 0.5f; p.height *= 0.5f; break;
        }
    }

    const Theme& th = u.theme();
    u.element()
     .color(color{ th.accent.r, th.accent.g, th.accent.b, 90 })
     .stroke(th.accent, 2)
     .radius(th.radius)
     .shape(string::shape::ROUNDED_RECTANGLE)
     .fixed(px(p.width), px(p.height))
     .floating(px(p.x), px(p.y))
     .overlay()
     .z(253);   // above every panel, below the debug surfaces
}

}  // namespace

WorkspaceView& WorkspaceView::content(const std::function<void(Ui&)>& declare)
{
    // 1. Collect declarations. `card()` is only meaningful while this flag is set.
    ui_.cards_.clear();
    ui_.declaring_cards_ = true;
    if (declare) declare(ui_);
    ui_.declaring_cards_ = false;

    // 2. Advance a card drag BEFORE emitting, so this frame's tree already reflects a completed
    // drop rather than showing the old arrangement for one frame.
    const interaction& ia = ui_.interaction_state();
    const Workspace::card_drag& d = ws_.dragging_card();
    if (d.active)
    {
        if (ia.is_active(d.element))
        {
            ws_.update_card_drag(ia.cursor_x, ia.cursor_y);
        }
        else
        {
            // Pointer capture ended: the drop lands wherever the cursor is. A drop over nothing
            // floats the card at the cursor, which is what makes tear-off just another drop.
            ws_.end_card_drag(panel_rect{ ia.cursor_x - 60.0f, ia.cursor_y - 10.0f, 320.0f, 240.0f });
        }
    }

    // 3. Walk the arrangement: the docked tree first, then floating panels back-to-front so their
    // z matches the order the workspace keeps.
    if (ws_.root() != no_node)
        emit_node(ui_, ws_, ws_.root(), direction::HORIZONTAL);

    const std::span<const node_index> floats = ws_.floating();
    for (std::size_t i = 0; i < floats.size(); ++i)
        emit_floating(ui_, ws_, floats[i], static_cast<std::uint8_t>(std::min<std::size_t>(i + 1, 250)));

    emit_drop_preview(ui_, ws_);

    ui_.cards_.clear();
    return *this;
}

}  // namespace string::ui
