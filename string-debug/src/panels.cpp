#include <string/debug/panels.hpp>

#include <algorithm>
#include <cstdio>

#include <string/core/logger.hpp>
#include <string/debug_draw.hpp>
#include <string/platform/input.hpp>
#include <string/vulkan/gpu_profiler.hpp>
#include <string/vulkan/graph_introspect.hpp>
#include <string/vulkan/lens.hpp>

#include <string/debug/debug_cvars.hpp>
#include <string/ui/theme.hpp>
#include <string/ui/widgets.hpp>

namespace string::debug
{
// Brief 14: the brief-06 debug surfaces are now `Ui::panel`s — they drag, resize, raise on click
// and clamp to the screen, none of which they could do as raw-builder elements at hardcoded
// positions (user: "I kinda prefer these windows to pop out as panels"). They therefore take their
// z from the PANEL STACK (1..N by front-to-back order) instead of the old fixed z=250, so a debug
// window can now be raised above or buried under a scene panel — which is what being a panel means.
using ::string::ui::theme;
using ::string::shape;
using ::string::make_id;
using ::string::fixed;
using ::string::grow;

namespace
{
::string::color line_color(ConsoleLine::Kind k)
{
    switch (k)
    {
        case ConsoleLine::Kind::Ok:    return theme().good;
        case ConsoleLine::Kind::Warn:  return theme().accent_warm;
        case ConsoleLine::Kind::Error: return theme().bad;
        case ConsoleLine::Kind::Echo:  return theme().accent;
        default:                       return theme().text;
    }
}

::string::color severity_color(::String::LogLevel lvl)
{
    switch (lvl)
    {
        case ::String::LogLevel::WARN:     return theme().accent_warm;
        case ::String::LogLevel::ERROR:
        case ::String::LogLevel::CRITICAL: return theme().bad;
        case ::String::LogLevel::DEBUG:
        case ::String::LogLevel::TRACE:    return theme().text_dim;
        default:                         return theme().text;
    }
}
}  // namespace

DebugPanels::DebugPanels() = default;

void DebugPanels::update_and_author(::string::ui::Ui& u, ::String::Input& input,
                                    ::String::InputMap& input_map)
{
    // begin_frame/end_frame belong to the APP now: one Ui per frame means one owner of its frame
    // boundary, and these surfaces are authored inside it like any other.
    const ::string::ui::interaction& ui = u.interaction_state();

    handle_system_actions(input_map);
    handle_toggles(input);

    // Modal: while the console is open, suppress gameplay input (camera/debug keys route through
    // InputMap, which honours text_capture). The console reads RAW input directly, so it still types.
    input.set_text_capture(console_open_);


    if (console_open_) handle_console_input(input);

    if (cv_menu_enabled().get()) author_menu_bar(u);

    if (console_open_) author_console(u);
    if (cv_logs_enabled().get()) author_logs(u);
    if (cv_graph_enabled().get()) author_graph(u);
    if (cv_dag_enabled().get()) author_dag(u);
    if (cv_image_enabled().get()) author_image(u);
    if (cv_bindings_enabled().get()) author_bindings(u, input, input_map);
    author_lenses(u, ui.screen);
    if (cv_hud_enabled().get()) author_hud(u);
    if (cv_inspector_enabled().get()) author_inspector(u);

    // Debug-draw self-test pattern (verifies the immediate-mode line pipeline end to end): RGB axes,
    // a wireframe AABB, and a wireframe sphere near the world origin, plus overlay variants above.
    if (cv_draw_test().get())
    {
        ::string::debug::axes(glm::mat4(1.0f), 2.0f);
        ::string::debug::aabb({ -1, 0, -1 }, { 1, 2, 1 }, { 0, 255, 0, 255 });
        ::string::debug::sphere({ 0, 3, 0 }, 1.0f, { 60, 160, 255, 255 });
        ::string::debug::aabb_overlay({ 1.5f, 0, -1 }, { 3.5f, 2, 1 }, { 255, 0, 255, 255 });
        ::string::debug::sphere_overlay({ 2.5f, 3, 0 }, 1.0f, { 255, 200, 0, 255 });
    }

    // FORWARD THIS Ui'S REQUESTS. DebugPanels owns a SECOND `Ui`, so the app's forwarding does not
    // reach it — the console's copy went nowhere until a user reported it. Same shape as the
    // `observe()` miss that broke the scroll extent: owning a Ui means owning its hand-offs, and
    // forgetting one is silent in both cases.
    // Consumed after ONE frame of authoring — long enough for the field to have skipped the
    // toggling keystroke, short enough that it never sticks. Clearing it on a key-state condition
    // instead left the field unfocused for as long as the console was open.
    swallow_keystroke_ = false;

}

// THE ONLY WAY TO CHANGE THE CONSOLE'S OPEN STATE.
//
// It is mirrored in two places — the member and the `dbg.console` CVar (so a headless capture or a
// console command can drive it) — and handle_toggles reconciles them CVar-first at the top of every
// frame. So a write that touches only the member is silently undone one frame later: that is exactly
// what happened when Escape's close set the member directly and the reconcile put it straight back.
//
// Two mirrors need one writer. This is it.
void DebugPanels::set_console_open(bool open)
{
    console_open_ = open;
    cv_console_open().set(open);
}

void DebugPanels::handle_toggles(const ::String::Input& in)
{

    // The console open state is mirrored in dbg.console so a headless capture (STRING_CONSOLE=1) or a
    // console command can drive it; the grave key toggles it at runtime. Reconcile both directions.
    //
    // EVERY write to `console_open_` MUST go through set_console_open — see its comment. Assigning
    // the member directly is what made Escape appear to do nothing: the CVar still said "open", and
    // this reconcile stomped the close back the very next frame.
    if (cv_console_open().get() != console_open_) console_open_ = cv_console_open().get();
    // Grave/backtick toggles the console (raw key, not InputMap, so it works while text_capture on).
    const bool grave = in.key_down(::String::KeyCode::GRAVE_ACCENT);
    if (grave && !prev_grave_)
    {
        set_console_open(!console_open_);
        // The SAME keypress also produces a text event, so without this the '`' that opened the
        // console gets typed INTO it. Swallowed by leaving the field unfocused for this one frame,
        // which is precise: the keystroke that toggled is consumed by the toggle, and a backtick is
        // still a character you can type afterwards. (The pre-TextField console filtered '`' out of
        // every keystroke forever, which banned the character to solve one frame's problem.)
        swallow_keystroke_ = true;
    }
    prev_grave_ = grave;
    cv_console_open().set(console_open_);

    // F2 -> HUD, F3 -> inspector, driving the CVars so console `dbg.hud`/`dbg.inspector` stay in sync.
    const bool f2 = in.key_down(::String::KeyCode::F2);
    if (f2 && !prev_f2_) cv_hud_enabled().set(!cv_hud_enabled().get());
    prev_f2_ = f2;
    const bool f3 = in.key_down(::String::KeyCode::F3);
    if (f3 && !prev_f3_) cv_inspector_enabled().set(!cv_inspector_enabled().get());
    prev_f3_ = f3;
    const bool f4 = in.key_down(::String::KeyCode::F4);
    if (f4 && !prev_f4_) cv_logs_enabled().set(!cv_logs_enabled().get());
    prev_f4_ = f4;
    const bool f5 = in.key_down(::String::KeyCode::F5);
    if (f5 && !prev_f5_) cv_graph_enabled().set(!cv_graph_enabled().get());
    prev_f5_ = f5;
    const bool f6 = in.key_down(::String::KeyCode::F6);
    if (f6 && !prev_f6_) cv_dag_enabled().set(!cv_dag_enabled().get());
    prev_f6_ = f6;
    const bool f7 = in.key_down(::String::KeyCode::F7);
    if (f7 && !prev_f7_) cv_image_enabled().set(!cv_image_enabled().get());
    prev_f7_ = f7;
    const bool f8 = in.key_down(::String::KeyCode::F8);
    if (f8 && !prev_f8_) cv_bindings_enabled().set(!cv_bindings_enabled().get());
    prev_f8_ = f8;
    const bool f1 = in.key_down(::String::KeyCode::F1);
    if (f1 && !prev_f1_) cv_menu_enabled().set(!cv_menu_enabled().get());
    prev_f1_ = f1;
}

void DebugPanels::handle_console_input(const ::String::Input& in)
{


    // TYPING, BACKSPACE, CARET, SELECTION AND CLIPBOARD ARE THE TEXT FIELD'S JOB — see
    // author_console. What stays here is CONSOLE semantics rather than text editing: history,
    // completion, and running the line.
    //
    // This used to hand-roll an editor of its own, including a byte-wise `pop_back()` backspace that
    // could split a UTF-8 character in half — the exact bug TextField had already fixed. A second
    // implementation of a shared problem drifts to the worse one; it just took a user reporting
    // "arrows don't work" to notice that the console was never using the widget at all.

    const bool enter = in.key_down(::String::KeyCode::ENTER);
    if (enter && !prev_enter_ && !input_.empty())
    {
        console_.execute(input_);
        input_.clear();
    }
    prev_enter_ = enter;

    const bool tab = in.key_down(::String::KeyCode::TAB);
    if (tab && !prev_tab_ && !input_.empty())
    {
        std::vector<std::string> matches = console_.complete(input_);
        if (matches.size() == 1)
        {
            input_ = matches[0] + " ";
        }
        else if (matches.size() > 1)
        {
            // Complete to the longest common prefix, then echo the candidates.
            std::string lcp = matches[0];
            for (const std::string& m : matches)
            {
                std::size_t i = 0;
                while (i < lcp.size() && i < m.size() && lcp[i] == m[i]) ++i;
                lcp.resize(i);
            }
            if (lcp.size() > input_.size()) input_ = lcp;
            std::string cand = "candidates:";
            for (std::size_t i = 0; i < matches.size() && i < 12; ++i) cand += " " + matches[i];
            console_.echo(cand);
        }
    }
    prev_tab_ = tab;

    const bool up = in.key_down(::String::KeyCode::UP);
    if (up && !prev_up_) { std::string h = console_.history_prev(); if (!h.empty()) input_ = h; }
    prev_up_ = up;
    const bool down = in.key_down(::String::KeyCode::DOWN);
    if (down && !prev_down_) input_ = console_.history_next();
    prev_down_ = down;
}

// The debug menu bar (brief 14 M1) — the home for global toggles that do not deserve panel real
// estate. Every entry here already had a CVar and a function key; the bar is a third way in that
// does not require knowing either, which is the point of a menu.
void DebugPanels::author_menu_bar(::string::ui::Ui& u)
{
    using namespace string::ui;
    auto bar = menu_bar(u, "dbg_menu");

    bar.menu("View", [&](Menu& m) {
        m.toggle("Console            `", bind(cv_console_open()));
        m.toggle("Profiler HUD      F2", bind(cv_hud_enabled()));
        m.toggle("Inspector         F3", bind(cv_inspector_enabled()));
        m.toggle("Logs              F4", bind(cv_logs_enabled()));
        m.toggle("Graph lifetimes   F5", bind(cv_graph_enabled()));
        m.toggle("Pass DAG          F6", bind(cv_dag_enabled()));
        m.toggle("Image browser     F7", bind(cv_image_enabled()));
        m.separator();
        // Creation is a menu ITEM, not a toggle: lenses are a list you add to, not a surface you
        // switch on. Silently no-ops at the cap rather than disappearing — a control that vanishes
        // reads as a bug.
        m.item("New lens", [] {
            ::String::LensState& ls = ::String::LensState::instance();
            if (ls.count >= ::String::LensState::kMaxLenses) return;
            // Created at 1x: correspondence is the default and zoom is opt-in, so a new lens shows
            // exactly what it covers until asked to do otherwise.
            ls.lenses[ls.count] = ::String::Lens{};
            ++ls.count;
        });
        m.separator();
        m.item("Close all", [] {
            cv_console_open().set(false);
            cv_hud_enabled().set(false);
            cv_inspector_enabled().set(false);
            cv_logs_enabled().set(false);
            cv_graph_enabled().set(false);
            cv_dag_enabled().set(false);
            cv_image_enabled().set(false);
        });
    });

    bar.menu("Debug", [&](Menu& m) {
        m.toggle("Debug-draw self test", bind(cv_draw_test()));
        m.separator();
        // Now resets EVERY panel, which is what it always claimed to do — it previously cleared
        // only the debug shell's own store and left the app's panels where they were.
        m.item("Reset panel layout", [&u] { u.panels().clear(); });
    });
}

void DebugPanels::author_console(::string::ui::Ui& u)
{
    using namespace string::ui;
    // MODAL, and it handles `cancel`: Escape closes it, and nav cannot walk out of it into the
    // panels behind. Both go through the routed-action path rather than a raw key check — which is
    // the point, because the key is now a player setting.
    Panel console = u.panel("dbg_console");
    console.title("Console")
        .modal()
        .handles(ui_action::cancel)
        .initial({ 16.0f, 16.0f, 760.0f, 330.0f })
        .limits({ 320.0f, 140.0f })
        .content([&](Ui& p) {
            p.text("Esc/` close · Tab complete · Up/Down history · Enter run · F4 logs")
             .color(theme().text_dim)
             .font(14);

            // Console command output only — the LOG TAIL is its own panel (F4/dbg.logs). Mixing
            // streams flooded the console and per-frame log arrivals resized/jittered the whole
            // surface (user). Command output changes only when a command runs.
            const std::deque<ConsoleLine>& out = console_.output();
            const std::size_t show = std::min<std::size_t>(10, out.size());
            for (std::size_t i = out.size() - show; i < out.size(); ++i)
                p.text(out[i].text).color(line_color(out[i].kind)).font(15).width(grow());

            // THE EDIT LINE IS A REAL TEXT FIELD. It was a plain text element with a "_" glued on
            // the end and its own editor behind it, which is why arrows and shift-selection did
            // nothing here however well they worked in the widget.
            //
            // `.focus(true)`: the console is modal and owns this field, so it is live the moment the
            // console opens — there is nothing to click.
            p.element().row().gap(4).align(alignment::CENTER).width(grow()).content([&](Ui& c) {
                c.text(">").color(theme().accent_warm).font(18);
                text_field(c, "dbg_console_edit", bind(*this, &DebugPanels::input_))
                    .focus(!swallow_keystroke_)
                    .width(700);
            });
        });

    // The router already decided this action belongs to the console — no "am I on top?" check here,
    // because that question is answered once, centrally, rather than by each surface guessing.
    if (console.took(ui_action::cancel))
        set_console_open(false);
}

// --- System actions: the ones nothing may swallow -----------------------------------------------
//
// `ctx.system` is pushed ONCE, above the UI's context, and claims its own actions. That placement is
// the whole mechanism: a modal dialog, the console, or a chat bar can each claim broadly without
// ever being able to take a screenshot key or a push-to-talk away from the player. Those must work
// while ANY surface is up — that is what makes them system actions rather than merely global ones.
//
// Claiming SELECTIVELY (not the exclusive overload) is equally load-bearing: this context must take
// its own actions off everyone below and nothing else. Reaching for the one-argument push here would
// suppress every gameplay key in the game, silently, from above.
void DebugPanels::handle_system_actions(::String::InputMap& map)
{
    using namespace ::String::literals;
    static constexpr ::String::ActionId kSystemContext = ::String::action_id("ctx.system");
    static constexpr ::String::ActionId kScreenshot    = ::String::action_id("system.screenshot");

    if (!system_bound_)
    {
        system_bound_ = true;
        map.bind_button("system.screenshot", ::String::KeyCode::F12);
        const ::String::ActionId claims[] = { kScreenshot };
        map.push_context(kSystemContext, claims, ::String::InputMap::kSystemPriority);
    }

    // Read AS the system context, so it fires through a modal, through text capture, through
    // anything. `r.capture.frame` is read live by the renderer; any already-passed frame index means
    // "next frame", and it resets itself to 0 after firing.
    if (map.pressed(kScreenshot, kSystemContext))
    {
        ::string::core::CVarRegistry::instance().set_from_string("r.capture.frame", "1");
        STRING_LOG_INFO("[bindings] screenshot requested (r.capture.path)");
    }
}

// --- Binding inspector + rebinder (F8 / dbg.bindings) -------------------------------------------
//
// The surface `name_of()` and `blocked()` were built for. It shows, per action, the name a human
// knows it by, what it is currently bound to, and whether some context above the base is CURRENTLY
// taking it — the last being the thing that is otherwise invisible: a binding that looks correct and
// silently does nothing because a surface above claimed it.
void DebugPanels::author_bindings(::string::ui::Ui& u, const ::String::Input& input,
                                  ::String::InputMap& map)
{
    using namespace string::ui;
    static constexpr ::String::ActionId kRebindContext = ::String::action_id("ctx.rebind");

    // WHILE CAPTURING A KEY, CLAIM EVERYTHING. Binding "W" would otherwise also walk the camera, and
    // binding Escape would close the panel you are binding from. This is the per-surface claim doing
    // real work rather than demonstrating itself: an exclusive push, popped the moment the capture
    // resolves, which is exactly the lifetime ScopedContext models — but the capture spans frames,
    // so it is an explicit push/pop pair instead.
    if (rebinding_.valid())
    {
        map.push_context(kRebindContext);   // exclusive: nothing below acts while we listen
        const ::String::KeyCode k = input.first_key_pressed();
        if (k != ::String::KeyCode::UNKNOWN)
        {
            // Escape cancels rather than binding — binding an escape hatch to Escape is how you lose
            // the ability to cancel.
            if (k != ::String::KeyCode::ESCAPE)
            {
                const std::string name{ map.name_of(rebinding_) };
                map.clear(rebinding_);
                if (!name.empty()) map.bind_button(name, k);
                else               map.bind_button(rebinding_, k);
                STRING_LOG_INFO("[bindings] {} -> {}", name, ::String::key_name(k));
            }
            rebinding_ = {};
            map.pop_context(kRebindContext);
        }
    }

    u.panel("dbg_bindings")
        .title("Input bindings")
        .initial({ 480.0f, 60.0f, 420.0f, 420.0f })
        .limits({ 320.0f, 160.0f })
        .content([&](Ui& p) {
            if (rebinding_.valid())
                p.text(p.own("Press a key for '" + std::string(map.name_of(rebinding_)) +
                             "'  (Esc cancels)"))
                 .color(theme().accent_warm).font(14).width(grow());
            else
                p.text("Click a binding to rebind it · blocked = a surface above is taking it")
                 .color(theme().text_dim).font(13).width(grow());

            const std::vector<::String::InputMap::action_info> all = map.actions();
            for (const auto& a : all)
            {
                // Describe every bound source, not just the first: an action bound to both a key and
                // a pad button is normal, and showing one of them would misreport the other as absent.
                std::string bound;
                for (const ::String::KeyCode k : a.keys)
                    bound += (bound.empty() ? "" : ", ") + std::string(::String::key_name(k));
                for (const ::String::MouseButton m : a.buttons)
                    bound += (bound.empty() ? "" : ", ") + std::string(::String::button_name(m));
                for (const ::String::GamepadButton g : a.pad)
                    bound += (bound.empty() ? "" : ", ") + std::string(::String::button_name(g));
                if (a.axis) bound = "(axis)";
                if (bound.empty()) bound = "unbound";

                // Blocked AT BASE: the question a player asks is "why doesn't my key work", and they
                // mean while playing.
                const bool blocked = map.blocked(a.id);
                const std::string_view label = a.name.empty() ? std::string_view{ "(unnamed)" } : a.name;

                Element row(p, ::string::id{ p.own("bind." + std::string(label)),
                                             ::string::make_id(p.own("bindrow." + std::string(label))).hash });
                const bool hot = p.interaction_state().is_hot(row.id_hash());
                row.color(p.theme().surface({ .hot = hot }))
                   .row().gap(8).width(grow()).height(fit()).pad(padding{ 6, 6, 3, 3 })
                   .focusable();
                row.content([&](Ui& c) {
                    c.text(label).color(blocked ? theme().text_dim : theme().text)
                     .font(14).width(grow());
                    c.text(c.own(bound)).color(theme().accent).font(14);
                    if (blocked)
                        c.text("blocked").color(theme().accent_warm).font(12);
                });
                // Axis actions are key PAIRS; rebinding one from a single key press would silently
                // drop the other half, so they are read-only here until the UI can capture a pair.
                if (row.clicked() && !a.axis && !rebinding_.valid())
                    rebinding_ = a.id;
            }
        });
}

// Log tail panel (F4 / dbg.logs) — deliberately SEPARATE from the console (user: reading logs and
// entering commands are different tasks; mixing them floods the console). Anti-jitter design: a
// FIXED number of single-line rows, newest FIRST, each row grow-width (single line, clipped at the
// panel edge). Row count and row height never change as logs arrive, so the panel geometry is
// rock-stable; long lines truncate (full text is in the terminal/log file).
void DebugPanels::author_logs(::string::ui::Ui& u)
{
    using namespace string::ui;
    u.panel("dbg_logs")
        .title("Logs — newest first")
        .initial({ 16.0f, 400.0f, 760.0f, 300.0f })
        .limits({ 320.0f, 120.0f })
        .content([&](Ui& p) {
            constexpr std::size_t kLogRows = 14;
            const std::vector<::String::LogRingBuffer::Line> log =
                ::String::LogRingBuffer::instance().tail(kLogRows);
            for (std::size_t i = 0; i < kLogRows; ++i)
            {
                // Newest first: index from the back of the tail. Missing rows render as empty lines
                // so the panel height is constant from the first frame.
                const bool have = i < log.size();
                const auto& ln = log[have ? log.size() - 1 - i : 0];
                p.text(have ? std::string_view{ ln.text } : std::string_view{ " " })
                 .color(have ? severity_color(ln.level) : theme().text_dim)
                 .font(15)
                 .width(grow());   // grow, not fixed: single line, clipped at the panel edge
            }
        });
}

// Render-graph RESOURCE LIFETIME TIMELINE (F5 / dbg.graph) — brief 14 M2.
//
// X is compiled pass position, one row per resource, a bar spanning [first..last]. It answers the
// question nothing else here does: which resources are alive when, and which spans OVERLAP. That is
// the aliasing input — brief 16 built the transient arena but deferred real interval packing, and
// this is the instrument for it.
//
// Deliberately NOT a graph: dependency structure is the pass DAG's job (M3). This is a chart of
// spans, so it is rows and boxes and needs no graph widget.
//
// FIRST DEBUG SURFACE ON THE FLUENT FACADE (user, 2026-08-01: "I kinda prefer these windows to pop
// out as panels so they can be dragged around and resized"). The brief-06 surfaces author into the
// raw builder at hardcoded positions, which is why none of them move; this one is a `Ui::panel`, so
// it drags, resizes, raises on click and clamps to the screen with no code of its own. The other
// four follow the same shape — see the note in debug_panels.hpp.
void DebugPanels::author_graph(::string::ui::Ui& u)
{
    using namespace string::ui;
    const ::String::GraphIntrospect* gi = ::String::GraphIntrospect::global();

    constexpr std::uint16_t kLabelW = 190;
    constexpr std::uint16_t kRowH = 15;
    constexpr float kChrome = 34.0f;   // body padding + the scrollbar-free right margin

    // The panel is resizable now, so the track width comes from its CURRENT rect rather than a
    // constant. Read from the store BEFORE authoring: `Panel::content` applies this frame's drag
    // internally, so what we see here is last frame's width — one frame stale during an active
    // resize drag, which is invisible at drag speed and the same staleness class as observed_box.
    const panel_rect initial{ 40.0f, 420.0f, 760.0f, 380.0f };
    const panel_state& st = u.panels().panel(make_id("dbg_graph").hash, initial);
    const auto track_w = static_cast<std::uint16_t>(
        std::max(120.0f, st.rect.width - static_cast<float>(kLabelW) - kChrome));

    u.panel("dbg_graph")
        .title("Graph — resource lifetimes")
        .initial(initial)
        .limits({ 360.0f, 160.0f })
        .content([&](Ui& p) {
            if (gi == nullptr || gi->passes.empty())
            {
                p.text("no compiled graph").color(theme().text_dim).font(15);
                return;
            }

            const std::size_t passes = gi->passes.size();
            // Integer column width, floored: the track is then passes*col wide, which may be a few
            // px short of track_w. Better a hair narrow than bars drifting off the last column from
            // accumulated rounding — reading which spans line up is the entire point.
            const int col = std::max(2, static_cast<int>(track_w / passes));

            // Header: pass position ticks. Names are far too long for a column each, so the axis is
            // numbered and the legend at the bottom maps number -> name.
            p.element().row().gap(0).content([&](Ui& r) {
                r.element().fixed(kLabelW, 1);
                for (std::size_t i = 0; i < passes; ++i)
                    r.text(r.own(std::to_string(i)))
                     .color(theme().text_dim)
                     .font(12)
                     .fixed(static_cast<std::uint16_t>(col), kRowH);
            });

            for (const ::String::GraphIntrospect::Resource& res : gi->resources)
            {
                p.element().row().gap(0).height(fixed(kRowH)).content([&](Ui& r) {
                    r.text(res.label)
                     .color(res.is_image ? theme().text : theme().text_dim)
                     .font(13)
                     .fixed(kLabelW, kRowH);

                    // Leading spacer then the bar: flow layout has no absolute child placement, and
                    // the bar's offset IS its first pass — the spacer trick the scrollbar uses.
                    const int lead = static_cast<int>(res.first) * col;
                    const int span = (static_cast<int>(res.last) - static_cast<int>(res.first) + 1) * col;
                    if (lead > 0) r.element().fixed(static_cast<std::uint16_t>(lead), 1);
                    // Images vs buffers read differently: the aliasing question is usually asked
                    // about images (they dominate the arena), so they are the foreground.
                    r.element()
                     .color(res.is_image ? theme().accent : theme().stroke_hi)
                     .radius(3)
                     .shape(shape::ROUNDED_RECTANGLE)
                     .fixed(static_cast<std::uint16_t>(std::max(2, span - 1)),
                            static_cast<std::uint16_t>(kRowH - 3));
                });
            }

            // The legend: position -> pass name, two per line so the panel is not a column of
            // mostly-empty rows.
            p.text("— passes —").color(theme().text_dim).font(13);
            for (std::size_t i = 0; i < passes; i += 2)
            {
                std::string line = std::to_string(i) + " " + gi->passes[i].name;
                if (i + 1 < passes)
                    line += "   " + std::to_string(i + 1) + " " + gi->passes[i + 1].name;
                p.text(p.own(std::move(line))).color(theme().text_dim).font(13).width(grow());
            }
        });
}

void DebugPanels::author_hud(::string::ui::Ui& u)
{
    using namespace string::ui;
    u.panel("dbg_hud")
        .title("Profiler HUD")
        .initial({ 16.0f, 720.0f, 300.0f, 300.0f })
        .limits({ 220.0f, 120.0f })
        .content([&](Ui& p) {
            // Per-pass GPU ms from the engine's always-on timestamp readback (global handle).
            const ::String::GpuProfiler* prof = ::String::GpuProfiler::global();
            char buf[96];
            if (prof && prof->enabled())
            {
                for (const auto& s : prof->stats())
                {
                    std::snprintf(buf, sizeof(buf), "%-16s %6.3f ms", s.name.c_str(), s.avg_ms);
                    p.text(p.own(std::string(buf))).color(theme().text).font(15).width(grow());
                }
                std::snprintf(buf, sizeof(buf), "GPU total       %6.3f ms", prof->total_avg_ms());
                p.text(p.own(std::string(buf))).color(theme().good).font(16);
            }
            else
            {
                p.text("gpu timing unavailable").color(theme().text_dim).font(15);
            }

            // Whatever renderer is running appends its own counters here. Generic timings above,
            // renderer-specific numbers below — this library never learns what they mean.
            if (hud_extra_) hud_extra_(p);
        });
}

void DebugPanels::author_inspector(::string::ui::Ui& u)
{
    using namespace string::ui;
    // The SLOT is generic — a titled, dockable panel on F3 with a stable id, so the menu bar and
    // the panel store work the same whoever fills it. The CONTENTS come from the renderer, because
    // only it knows what a draw is (see string-render-forward's RenderDebug).
    u.panel("dbg_inspector")
        .title("Scene / draw inspector")
        .initial({ 820.0f, 16.0f, 360.0f, 460.0f })
        .limits({ 280.0f, 140.0f })
        .content([&](Ui& p) {
            if (inspector_) inspector_(p);
            else p.text("no inspector registered").color(theme().text_dim).font(15);
        });
}

// M3 — the pass DAG, with provenance by selection.
//
// A real node canvas: this container computes its children's coordinates itself and places them
// with `.floating(x, y).local()`. layout.hpp's `float_local` was built for exactly this ("a graph
// canvas laying out nodes, for instance") — the first draft of this panel drew rows of chips with
// no edges on the assumption that absolute placement did not exist, which was wrong and did not
// read as a graph.
//
// Rows are LONGEST-PATH DEPTH, not execution order. Passes sharing a row are genuinely independent
// (neither can reach the other), which is the structural fact execution order hides — the toposort
// picks one arbitrary valid sequence out of many. Edges are Manhattan-routed through the channel
// below each row and drawn BEFORE the nodes, so a long edge passing a row slides behind it.
void DebugPanels::author_dag(::string::ui::Ui& u)
{
    using namespace ::string::ui;
    const ::String::GraphIntrospect* gi = ::String::GraphIntrospect::global();

    // Panel width drives the node width, so read the stored rect before authoring — one frame stale
    // during an active resize drag, invisible at drag speed (same class as the timeline panel).
    const panel_rect initial{ 40.0f, 60.0f, 660.0f, 460.0f };
    const panel_state& pst = u.panels().panel(make_id("dbg_dag").hash, initial);
    const float avail = std::max(200.0f, pst.rect.width - 34.0f);

    u.panel("dbg_dag")
        .title("Graph — pass DAG")
        .initial(initial)
        .limits({ 340.0f, 200.0f })
        .content([&](Ui& p) {
            if (gi == nullptr || gi->passes.empty())
            {
                p.text("no compiled graph").color(theme().text_dim).font(15);
                return;
            }
            const auto& passes = gi->passes;
            const int np = static_cast<int>(passes.size());
            if (sel_pass_ >= np) sel_pass_ = -1;
            if (sel_res_ >= static_cast<int>(gi->resources.size())) sel_res_ = -1;

            // Reverse edges, rebuilt each frame: the snapshot stores successors only, and scanning
            // ~30 passes is far cheaper than a second array that can fall out of sync.
            std::vector<std::vector<std::uint32_t>> preds(passes.size());
            for (std::uint32_t i = 0; i < passes.size(); ++i)
                for (std::uint32_t s : passes[i].successors)
                    if (s < preds.size()) preds[s].push_back(i);

            // Column index within each depth row, and the widest row (which sets the node width).
            std::uint32_t max_depth = 0;
            for (const auto& q : passes) max_depth = std::max(max_depth, q.depth);
            std::vector<int> col(passes.size(), 0);
            std::vector<int> per_row(max_depth + 1, 0);
            for (std::size_t i = 0; i < passes.size(); ++i)
                col[i] = per_row[passes[i].depth]++;
            const int widest = *std::max_element(per_row.begin(), per_row.end());

            constexpr int kNodeH = 22;
            constexpr int kChannel = 26;          // vertical space between rows, for edge routing
            constexpr int kGapX = 8;
            const int node_w = std::clamp(
                static_cast<int>((avail - (widest - 1) * kGapX) / std::max(1, widest)), 40, 160);
            const int pitch_x = node_w + kGapX;
            const int pitch_y = kNodeH + kChannel;
            const auto nx = [&](std::size_t i) { return col[i] * pitch_x; };
            const auto ny = [&](std::size_t i) { return static_cast<int>(passes[i].depth) * pitch_y; };

            // Does pass `i` touch the selected resource, and how?
            const auto touches = [&](int i, bool& writes) {
                writes = false;
                if (sel_res_ < 0) return false;
                bool any = false;
                for (const auto& use : passes[static_cast<std::size_t>(i)].uses)
                    if (static_cast<int>(use.resource) == sel_res_)
                    {
                        any = true;
                        writes = writes || use.write;
                    }
                return any;
            };
            const auto related = [&](int i) {
                if (sel_pass_ < 0) return false;
                if (i == sel_pass_) return true;
                const auto& sp = passes[static_cast<std::size_t>(sel_pass_)];
                if (std::find(sp.successors.begin(), sp.successors.end(),
                              static_cast<std::uint32_t>(i)) != sp.successors.end()) return true;
                const auto& pr = preds[static_cast<std::size_t>(sel_pass_)];
                return std::find(pr.begin(), pr.end(), static_cast<std::uint32_t>(i)) != pr.end();
            };

            const ::String::GpuProfiler* prof = ::String::GpuProfiler::global();
            const auto ms_of = [&](const std::string& name) -> float {
                if (!prof || !prof->enabled()) return -1.0f;
                for (const auto& s : prof->stats())
                    if (s.name == name) return s.avg_ms;
                return -1.0f;
            };

            p.text(p.own(std::to_string(np) + " passes · " + std::to_string(gi->resources.size())
                         + " resources · rows = dependency depth"))
             .color(theme().text_dim).font(13).width(grow());
            // TWO scroll regions, not one. The first draft put the graph and the resource list in a
            // single viewport, so scrolling the list scrolled the graph away with it — and reading
            // "what does this pass touch" while the pass you selected has slid off the top is
            // useless. They are separate views of the same selection, so each gets its own extent:
            // the graph holds its place while the list scrolls under it.
            const float body = std::clamp(pst.rect.height - 96.0f, 120.0f, 4000.0f);
            const auto list_h = static_cast<std::uint16_t>(std::max(60.0f, body * 0.4f));
            const auto canvas_view_h = static_cast<std::uint16_t>(std::max(80.0f, body - list_h));

            scroll_area(p, "dag_canvas").size(static_cast<std::uint16_t>(avail), canvas_view_h)
                .content([&](Ui& s) {
                // widest*pitch_x would include a TRAILING gap, pushing the canvas a few px past the
            // viewport and clipping the last node of the widest row. The canvas is nodes plus the
            // gaps BETWEEN them.
            const int canvas_w = widest * node_w + (widest - 1) * kGapX;
                const int canvas_h = (static_cast<int>(max_depth) + 1) * pitch_y;

                s.element()
                 .fixed(static_cast<std::uint16_t>(std::min(canvas_w, 4000)),
                        static_cast<std::uint16_t>(std::min(canvas_h, 4000)))
                 .content([&](Ui& c) {
                    // --- edges first, so nodes paint over them ------------------------------------
                    const auto seg = [&](int x, int y, int w, int h, ::string::color col_) {
                        c.element()
                         .floating(x, y).local()
                         .color(col_)
                         .fixed(static_cast<std::uint16_t>(std::max(1, w)),
                                static_cast<std::uint16_t>(std::max(1, h)));
                    };
                    for (std::size_t i = 0; i < passes.size(); ++i)
                    {
                        for (std::uint32_t j : passes[i].successors)
                        {
                            if (j >= passes.size()) continue;
                            const bool hot = sel_pass_ >= 0
                                           && (static_cast<int>(i) == sel_pass_
                                               || static_cast<int>(j) == sel_pass_);
                            const ::string::color ec = hot ? theme().accent : theme().stroke;
                            const int t = hot ? 2 : 1;

                            const int sx = nx(i) + node_w / 2;
                            const int sy = ny(i) + kNodeH;
                            const int tx = nx(j) + node_w / 2;
                            const int ty = ny(j);
                            const int my = sy + kChannel / 2;
                            seg(sx, sy, t, my - sy, ec);                                  // drop
                            seg(std::min(sx, tx), my, std::abs(tx - sx) + t, t, ec);      // run
                            seg(tx, my, t, ty - my, ec);                                  // rise
                        }
                    }

                    // --- nodes --------------------------------------------------------------------
                    for (int i = 0; i < np; ++i)
                    {
                        const auto si = static_cast<std::size_t>(i);
                        ::string::color cc = theme().text_dim;
                        bool strong = false;
                        if (sel_pass_ >= 0)
                        {
                            if (i == sel_pass_) { cc = theme().accent; strong = true; }
                            else if (related(i))
                            {
                                const auto& sp = passes[static_cast<std::size_t>(sel_pass_)];
                                const bool succ = std::find(sp.successors.begin(), sp.successors.end(),
                                                            static_cast<std::uint32_t>(i))
                                                  != sp.successors.end();
                                cc = succ ? theme().accent_warm : theme().good;
                                strong = true;
                            }
                        }
                        else if (sel_res_ >= 0)
                        {
                            bool w = false;
                            if (touches(i, w)) { cc = w ? theme().accent : theme().accent_warm; strong = true; }
                        }

                        Element node(c, make_id(c.own("dag.p" + std::to_string(i))));
                        node.floating(nx(si), ny(si)).local()
                            .color(strong ? theme().panel : theme().panel_alt)
                            .stroke(cc, strong ? 2 : 1)
                            .radius(4)
                            .shape(::string::shape::ROUNDED_RECTANGLE)
                            .pad(padding{ 4, 4, 2, 2 })
                            .clip()
                            .fixed(static_cast<std::uint16_t>(node_w), static_cast<std::uint16_t>(kNodeH))
                            .focusable();
                        node.content([&](Ui& u2) {
                            // Truncate rather than wrap: a node is a fixed box, and a wrapped name would
                            // change the row pitch that the edge routing is computed against.
                            std::string label = std::to_string(i) + " " + passes[si].name;
                            const auto budget = static_cast<std::size_t>(std::max(3, (node_w - 8) / 6));
                            if (label.size() > budget) label = label.substr(0, budget - 1) + "…";
                            u2.text(u2.own(std::move(label))).color(cc).font(12);
                            const float ms = ms_of(passes[si].name);
                            if (ms >= 0.05f && node_w >= 96)
                            {
                                char b[24];
                                std::snprintf(b, sizeof(b), "%.2f", ms);
                                u2.text(u2.own(std::string(b))).color(theme().text_dim).font(10);
                            }
                        });
                        if (node.clicked())
                        {
                            sel_pass_ = (sel_pass_ == i) ? -1 : i;   // click again to clear
                            sel_res_ = -1;
                        }
                    }
                 });

                });

            scroll_area(p, "dag_list").size(static_cast<std::uint16_t>(avail), list_h)
                .content([&](Ui& s) {
                // --- provenance ---------------------------------------------------------------------
                if (sel_pass_ >= 0)
                {
                    const auto& sp = passes[static_cast<std::size_t>(sel_pass_)];
                    s.text(s.own("selected: " + sp.name)).color(theme().accent).font(14).width(grow());
                    s.text(s.own(std::to_string(preds[static_cast<std::size_t>(sel_pass_)].size())
                                 + " feed it (green) · " + std::to_string(sp.successors.size())
                                 + " fed by it (amber)"))
                     .color(theme().text_dim).font(12).width(grow());
                    for (const auto& use : sp.uses)
                    {
                        if (use.resource >= gi->resources.size()) continue;
                        const auto& res = gi->resources[use.resource];
                        Element row(p, make_id(s.own("dag.u" + std::to_string(sel_pass_) + "."
                                                     + std::to_string(use.resource)
                                                     + (use.write ? "w" : "r"))));
                        row.width(grow()).height(fit()).fit().focusable();
                        row.content([&](Ui& u2) {
                            u2.text(use.write ? "W" : "R")
                              .color(use.write ? theme().accent_warm : theme().text_dim)
                              .font(12).fixed(14, 15);
                            u2.text(res.label).color(theme().text).font(12);
                        });
                        // Click a resource row to pivot selection onto that resource — the navigation
                        // model is pass -> its resources -> everything else touching one.
                        if (row.clicked()) { sel_res_ = static_cast<int>(use.resource); sel_pass_ = -1; }
                    }
                }
                else if (sel_res_ >= 0)
                {
                    const auto& res = gi->resources[static_cast<std::size_t>(sel_res_)];
                    s.text(s.own("selected: " + res.label + (res.is_image ? "  (image)" : "  (buffer)")))
                     .color(theme().accent).font(14).width(grow());
                    std::string writers, readers;
                    for (int i = 0; i < np; ++i)
                    {
                        bool w = false;
                        if (!touches(i, w)) continue;
                        std::string& into = w ? writers : readers;
                        if (!into.empty()) into += ", ";
                        into += passes[static_cast<std::size_t>(i)].name;
                    }
                    s.text(s.own("produced by (accent): " + (writers.empty() ? "— external input" : writers)))
                     .color(theme().text).font(12).width(grow()).wrap();
                    s.text(s.own("consumed by (amber): " + (readers.empty() ? "—" : readers)))
                     .color(theme().text).font(12).width(grow()).wrap();
                }
                else
                {
                    s.text("click a node to trace what feeds it and what it feeds")
                     .color(theme().text_dim).font(12).width(grow());
                }

                // What the DAG cannot show, said out loud rather than silently omitted: a toggled-off or
                // transitively-skipped pass is not in the compiled plan at all, so it has no node.
                if (!gi->dropped.empty())
                {
                    std::string line = "not in this frame: ";
                    for (std::size_t i = 0; i < gi->dropped.size(); ++i)
                        line += (i ? ", " : "") + gi->dropped[i];
                    s.text(s.own(std::move(line))).color(theme().text_dim).font(12).width(grow()).wrap();
                }
                });
        });
}

// M4 — the image widget, proven against the glyph atlas.
//
// The brief's de-risking, followed exactly: the widget, its shader and its pipeline are built and
// verified against the DYNAMIC GLYPH ATLAS, which is already a bindless sampled texture with a
// stable slot. That means zero barrier risk, no dynamically-declared SampledRead, and no
// target-absent-this-frame failure mode — while still exercising every control that matters.
//
// The controls are not decoration. An SDF atlas is a single R8 channel whose interesting structure
// lives in a narrow band around 0.5; blitted raw it is flat grey. Range remap is what makes the
// glyph edges visible, the channel mask is what proves only R carries data, and false colour is what
// turns "vaguely light and dark" into a readable gradient. Those are exactly the controls a depth
// target or a packed G-buffer will need, which is the point of proving them here first.
void DebugPanels::author_image(::string::ui::Ui& u)
{
    using namespace ::string::ui;

    u.panel("dbg_image")
        .title("Image — glyph atlas")
        .initial({ 700.0f, 500.0f, 380.0f, 460.0f })
        .limits({ 260.0f, 240.0f })
        .content([&](Ui& p) {
            static const std::string_view kChannels[] = { "RGBA", "R", "G", "B", "A" };
            // Index -> mask, rather than the mask itself, because a combo binds an int and the
            // masks are not contiguous.
            static const std::uint8_t kMasks[] = { 0xF, 1, 2, 4, 8 };

            combo(p, "img_ch", bind(*this, &DebugPanels::img_channel_))
                .label("Channels")
                .options(kChannels);
            slider(p, "img_mip", bind(*this, &DebugPanels::img_mip_))
                .label("Mip").range(0.0f, 7.0f).precision(0);
            slider(p, "img_lo", bind(*this, &DebugPanels::img_min_))
                .label("Range min").range(0.0f, 1.0f).precision(2);
            slider(p, "img_hi", bind(*this, &DebugPanels::img_max_))
                .label("Range max").range(0.0f, 1.0f).precision(2);
            checkbox(p, "img_fc", bind(*this, &DebugPanels::img_false_colour_))
                .label("False colour");

            ::string::image_run run{};
            run.source = ::string::image_source::glyph_atlas;
            run.channels = kMasks[std::clamp(img_channel_, 0, 4)];
            run.mip = static_cast<std::uint8_t>(std::lround(img_mip_));
            run.false_colour = img_false_colour_ ? 1u : 0u;
            run.range_min = img_min_;
            run.range_max = img_max_;

            // Square-ish viewport: the atlas is square, and letting the element grow on both axes
            // would stretch it to the panel's aspect and misrepresent what is in it.
            const panel_state& st = p.panels().panel(make_id("dbg_image").hash,
                                                     { 700.0f, 500.0f, 380.0f, 460.0f });
            const auto side = static_cast<std::uint16_t>(
                std::clamp(std::min(st.rect.width - 34.0f, st.rect.height - 210.0f), 64.0f, 1024.0f));

            p.element()
             .stroke(theme().stroke, 1)
             .fixed(side, side)
             .content([&](Ui& c) { c.element().image(run).fixed(side, side); });

            if (img_max_ <= img_min_)
                p.text("range max <= min — showing raw values")
                 .color(theme().accent_warm).font(12).width(grow());
        });
}

// M5 — the lens: chrome + controls. The SUBSTITUTION happens in the composite pass; the UI's whole
// job is a draggable rect and a few sliders, which is the point of putting it there.
//
// N lenses, not one (decided with the user): two lenses can put different views side by side, or one
// at 1x for correspondence beside one at 8x for detail — workflows a single lens cannot do at all.
// The DATA LAYOUT carries N now (LensState::kMaxLenses); this ships the UI for N, created one at a
// time from View ▸ New lens.
//
// The rect IS a panel_state driven by update_panel, so move/resize/clamp and the origin-capture
// anti-drift discipline come for free — no new drag code, and no second implementation to keep in
// step with the one that took several corrections to get right.
void DebugPanels::author_lenses(::string::ui::Ui& u, ::string::dimension screen)
{
    using namespace ::string::ui;
    ::String::LensState& ls = ::String::LensState::instance();

    // Headless verification lever (dbg.lens_test): one deterministic lens, so a capture can prove
    // the composite actually substitutes. Seeded ONCE — a starting state, not a per-frame override,
    // so it can still be dragged and tuned after it appears.
    static bool lens_test_seeded = false;
    if (cv_lens_test().get() > 0 && !lens_test_seeded && ls.count == 0)
    {
        lens_test_seeded = true;
        ls.lenses[0] = ::String::Lens{ 300.0f, 200.0f, 300.0f, 240.0f,
                                       static_cast<float>(cv_lens_test().get()), 0u, false };
        ls.count = 1;
        u.panels().panel(make_id("lens0").hash, { 300.0f, 200.0f, 300.0f, 240.0f });
    }

    // CHROME LIVES OUTSIDE THE SUBSTITUTED RECT.
    //
    // The UI draws into the HDR target BEFORE the composite runs, and the composite then REPLACES
    // every pixel of the lens rect with magnified content. So chrome drawn on top of the rect is
    // sampled away the instant magnification is anything but 1 — the outline, strip and grip
    // vanished and the lens became ungrabbable until zoom was reset.
    //
    // Putting the chrome in the ring just OUTSIDE the rect fixes it at every magnification, and is
    // better anyway: nothing overlaps the pixels being inspected, which is the whole point.
    constexpr int kEdge = 2;    // outline thickness
    constexpr int kStrip = 12;  // move strip, above the rect
    constexpr int kGrip = 14;   // resize grip, past the bottom-right corner

    // ...which is exactly why the clamp has to be told about it. The move strip is the one piece of
    // lens chrome that lives OUTSIDE the rect, so clamping the rect to the viewport would still let
    // the strip slide off the top edge and leave the lens ungrabbable.
    constexpr panel_header kLensHeader{ -kEdge, -(kEdge + kStrip), 2 * kEdge, kStrip };

    // --- on-frame chrome: an outline and a grip, nothing else. The entire point is seeing the
    // pixels underneath, so anything more opaque would defeat the tool.
    for (std::uint32_t i = 0; i < ls.count; ++i)
    {
        const std::string base = "lens" + std::to_string(i);
        const std::uint64_t move_id = make_id(u.own(base + ".move")).hash;
        const std::uint64_t grip_id = make_id(u.own(base + ".grip")).hash;

        panel_state& st = u.panels().panel(make_id(u.own(base)).hash,
                                           { 220.0f + 40.0f * static_cast<float>(i),
                                             220.0f + 40.0f * static_cast<float>(i), 260.0f, 200.0f });
        // Resolve the drag BEFORE emitting: the rect this produces is the one to draw at, so the
        // outline never lags the cursor by a frame.
        update_panel(st, u.interaction_state(), panel_handles{ move_id, grip_id },
                     panel_limits{ 48.0f, 48.0f, kLensHeader }, screen);

        // Publish to the pass. This is the UI -> pass direction; the composite reads it later this
        // same frame, on the same thread.
        ls.lenses[i].x = st.rect.x;
        ls.lenses[i].y = st.rect.y;
        ls.lenses[i].w = st.rect.width;
        ls.lenses[i].h = st.rect.height;

        // The outline is the lens's SURFACE; the strip and the grip ride it in its coordinates.
        //
        // All three used to be root-space siblings, each re-deriving `st.rect` — the note here said
        // a child "would be positioned relative to the frame's box and clipped to it", and only the
        // first half was ever true: clipping requires `.clip`, which nothing on this frame sets.
        // Being positioned relative to the frame is precisely what is wanted, now that local
        // coordinates can say it: dragging a lens writes ONE placement instead of three that had to
        // agree, and the chrome cannot drift from the outline because it no longer knows where the
        // outline is.
        const auto frame_w = static_cast<std::uint16_t>(st.rect.width + 2 * kEdge);
        const auto frame_h = static_cast<std::uint16_t>(st.rect.height + 2 * kEdge);

        Element frame(u, id{ u.own(base + ".frame"), make_id(u.own(base + ".frame")).hash });
        frame.floating(static_cast<int>(st.rect.x) - kEdge, static_cast<int>(st.rect.y) - kEdge)
             .layer(ui_layer::popups)
             .z(0)
             .stroke(theme().accent, kEdge)
             .fixed(frame_w, frame_h);

        frame.content([&](Ui& l) {
            // Above the outline — a negative local anchor, which is exactly what signed positions
            // are for. The strip spans the OUTLINE's width, not the rect's: sized to the rect it
            // sat inset by the 2px edge on each side, so the border overhung the bar and the two
            // read as separate pieces of chrome instead of one frame.
            Element bar(l, id{ l.own(base + ".move"), move_id });
            bar.floating(0, -kStrip)
               .local()
               .color(::string::color{ theme().accent.r, theme().accent.g, theme().accent.b, 160 })
               .fixed(frame_w, kStrip);

            Element grip(l, id{ l.own(base + ".grip"), grip_id });
            grip.floating(frame_w, frame_h)
                .local()
                .color(theme().accent)
                .radius(3)
                .shape(::string::shape::ROUNDED_RECTANGLE)
                .fixed(kGrip, kGrip);
        });
    }

    // --- controls: a DOCKED card, one row per lens — not floating over the viewport, which would
    // put opaque chrome exactly where the pixels of interest are.
    if (ls.count == 0) return;
    u.panel("dbg_lens")
        .title("Lenses")
        .initial({ 16.0f, 500.0f, 320.0f, 200.0f })
        .limits({ 240.0f, 120.0f })
        .content([&](Ui& p) {
            for (std::uint32_t i = 0; i < ls.count; ++i)
            {
                const std::string base = "lens" + std::to_string(i);
                p.text(p.own("lens " + std::to_string(i))).color(theme().accent).font(13).width(grow());
                slider(p, p.own(base + ".mag"), bind(ls.lenses[i], &::String::Lens::magnification))
                    .label("Magnify").range(1.0f, 16.0f).precision(1);
                checkbox(p, p.own(base + ".raw"), bind(ls.lenses[i], &::String::Lens::bypass_tonemap))
                    .label("Bypass tonemap");
                Element close(p, make_id(p.own(base + ".close")));
                close.color(theme().panel_alt).stroke(theme().stroke, 1).radius(3)
                     .shape(::string::shape::ROUNDED_RECTANGLE).pad(padding{ 6, 6, 2, 2 }).fit()
                     .focusable();
                close.content([&](Ui& c) { c.text("close").color(theme().text_dim).font(12); });
                if (close.clicked())
                {
                    // Compact rather than leave a hole: the shader loops over [0, count), so a
                    // gap would silently show a stale lens.
                    for (std::uint32_t j = i; j + 1 < ls.count; ++j) ls.lenses[j] = ls.lenses[j + 1];
                    if (ls.count > 0) --ls.count;
                    break;
                }
            }
            p.text(p.own(std::to_string(ls.count) + " / "
                         + std::to_string(::String::LensState::kMaxLenses) + " — View ▸ New lens"))
             .color(theme().text_dim).font(12).width(grow());
        });
}

}  // namespace string::debug
