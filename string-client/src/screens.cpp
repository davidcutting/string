#include <string/client/screens.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include <string/client/theme.hpp>

#include <string/ui/widgets.hpp>

namespace string::client
{
using namespace string;

namespace
{

// A horizontal bar (rounded) with a proportional fill child, as a floating overlay subtree at (x,y).
// width/height in px; `frac` 0..1. Colours pre-faded by the caller.
void bar(Ui& u, uint16_t x, uint16_t y, uint16_t w, uint16_t h, float frac, color bg, color fill)
{
    frac = std::clamp(frac, 0.0f, 1.0f);
    u.element()
        .floating(x, y)
        .color(bg)
        .radius(h / 2)
        .shape(shape::ROUNDED_RECTANGLE)
        .fixed(w, h)
        .row()
        .content([&](Ui& u2) {
            u2.element()
                .color(fill)
                .radius(h / 2)
                .shape(shape::ROUNDED_RECTANGLE)
                .fixed(static_cast<uint16_t>(std::max(1.0f, frac * w)), h);
        });
}

color fade(color c, float a)
{
    return color{ c.r, c.g, c.b, static_cast<uint8_t>(std::clamp(c.a * a, 0.0f, 255.0f)) };
}

}  // namespace

void author_nameplates(Ui& u, const UiScene& scene, std::size_t budget)
{
    const std::size_t n = budget == 0 ? scene.anchors.size() : std::min(budget, scene.anchors.size());

    for (std::size_t i = 0; i < n; ++i)
    {
        const UiAnchor& a = scene.anchors[i];
        glm::vec2 px;
        float depth;
        if (!scene.project(a.world, px, depth))
            continue;

        // Distance fade + scale: near = full, far = smaller/dimmer. depth is clip-space w (view dist).
        const float t = std::clamp((depth - 4.0f) / 40.0f, 0.0f, 1.0f);
        const float alpha = 1.0f - t * 0.75f;
        const uint16_t bar_w = static_cast<uint16_t>(std::lround(110.0f - t * 50.0f));
        const uint16_t font_px = static_cast<uint16_t>(std::lround(22.0f - t * 8.0f));

        const float x = px.x - bar_w * 0.5f;
        const float y = px.y - 34.0f;
        if (x < -bar_w || x > scene.screen.x || y < -40.0f || y > scene.screen.y)
            continue;
        const uint16_t ix = static_cast<uint16_t>(std::clamp(x, 0.0f, scene.screen.x));
        const uint16_t iy = static_cast<uint16_t>(std::clamp(y, 0.0f, scene.screen.y));

        // Name label (floating text), colour by faction, pre-faded. The facade's arena keeps the
        // string alive for the frame, so no per-screen scratch bookkeeping.
        u.text(a.name)
            .floating(ix, iy)
            .color(fade(a.hostile ? col::hostile : col::friendly, alpha))
            .font(font_px);

        // Health bar just below the name.
        bar(u, ix, static_cast<uint16_t>(iy + font_px + 2), bar_w, 8, a.health,
            fade(col::hp_bg, alpha), fade(col::hp_fill, alpha));

        // Cast bar (only while casting) below the health bar.
        if (a.cast >= 0.0f)
        {
            bar(u, ix, static_cast<uint16_t>(iy + font_px + 13), bar_w, 6, a.cast,
                fade(col::hp_bg, alpha), fade(col::cast_fill, alpha));
        }
    }
}

// (5) Floating panels (brief 12 M1). Three panels, deliberately overlapping at their default
// positions so click-to-raise is visible: pressing a buried one — its chrome OR a widget inside it —
// brings it to the front, and the raise takes effect on the next frame.
//
// MINIMUM SIZES ARE THE AUTHOR'S JOB, deliberately. The engine's floor is a plain constant that
// knows nothing about content, so a panel can otherwise be resized smaller than what it holds. The
// alternative — deriving the floor from the measured content — would need a post-layout writeback
// into PanelStore, which buys a new frame-ordering constraint (the bug class that produced the
// froxel device-lost crash and the ensure_hiz VUID) and mixes derived data into a store that
// currently means "where the user dragged it". Not worth it, because it is interim work: once text
// wrap and scrolling exist, "smaller than its contents" stops being a defect and becomes the case
// scrolling is FOR. Until then the failure is visible and local — glyphs clip to their own node box
// (ui_pass.cpp), so a bad minimum truncates text inside the panel rather than spilling over
// anything. See docs/briefs/12-ui-panels.md, "Overflow policy".
void author_panels(Ui& u, ScreenState& state)
{
    u.panel("panel_stats")
        .title("Stats")
        // Widest line is 191px + 24px body padding; 4 rows + gaps + the 38px title bar ≈ 166px.
        .limits({ 220.0f, 170.0f })
        .initial({ 60.0f, 90.0f, 320.0f, 220.0f })
        .content([&](Ui& u2) {
            u2.text(u2.own("frame " + std::to_string(state.frame)));
            u2.text(u2.own("time  " + std::to_string(static_cast<int>(state.time)) + "s"));
            u2.text("drag the title bar").color(theme().text_dim);
            u2.text("resize from the corner").color(theme().text_dim);
        });

    u.panel("panel_controls")
        .title("Controls")
        // The caption row is 216px wide; the two buttons stack to ≈166px with the title bar.
        .limits({ 240.0f, 170.0f })
        .initial({ 300.0f, 220.0f, 300.0f, 200.0f })
        .content([&](Ui& u2) {
            u2.text("buttons still work inside a panel").color(theme().text_dim).font(16);
            u2.button("panel_btn_a").content([](Ui& u3) { u3.text("Alpha"); });
            u2.button("panel_btn_b").content([](Ui& u3) { u3.text("Beta"); });
        });

    // Text wrap, in the case it exists for: a paragraph in a GROW box, whose width is not decided
    // until the layout's width pass has run — so it re-flows as you drag the panel's edge. A
    // fixed-width text element has wrapped since long before `.wrap()`; this is the one that could
    // not.
    u.panel("panel_notes")
        .title("Notes")
        .initial({ 300.0f, 90.0f, 280.0f, 180.0f })
        .limits({ 140.0f, 120.0f })
        .content([&](Ui& u2) {
            u2.element()
              .text("Drag the right edge and this paragraph re-flows: it takes whatever width the "
                    "layout gives it and asks for the height its lines need.")
              .color(theme().text_dim)
              .font(16)
              .wrap()
              .width(grow());
        });

    // The clamp demo: its content really does fit in 160x96 (127x82), so this floor is honest and
    // dragging the grip inward stops exactly at a size the panel can still display.
    u.panel("panel_tiny")
        .title("Tiny")
        .initial({ 560.0f, 120.0f, 200.0f, 140.0f })
        .limits({ 160.0f, 96.0f })
        .content([&](Ui& u2) { u2.text("min 160x96").color(theme().text_dim); });
}

void seed_workspace(::string::ui::Workspace& ws)
{
    using namespace string::ui;
    ws.dock(kCardTree.hash, left());                                  // becomes the root
    ws.dock(kCardStats.hash, left(fixed(240)).locked());              // fixed strip, edge not draggable
    ws.dock(kCardLog.hash, bottom_of(kCardTree.hash, percent(500)));
    ws.dock(kCardProps.hash, tab_with(kCardLog.hash));                // shares Log's panel as a tab
}

void author_workspace(Ui& u, ::string::ui::Workspace& ws, ScreenState& state)
{
    u.workspace(ws).content([&](Ui& u2) {
        u2.card(kCardStats).title("Stats").content([&](Ui& u3) {
            u3.text(u3.own("frame " + std::to_string(state.frame)));
            u3.text("this strip is locked").color(theme().text_dim);
        });
        u2.card(kCardTree).title("Tree").content([&](Ui& u3) {
            for (int i = 0; i < 4; ++i)
                u3.text(u3.own("node " + std::to_string(i))).color(theme().text_dim);
        });
        u2.card(kCardLog).title("Log").content([&](Ui& u3) {
            u3.text("drag the splitter above").color(theme().text_dim);
        });
        u2.card(kCardProps).title("Props").content([&](Ui& u3) {
            u3.text("tabbed with Log").color(theme().text_dim);
        });
    });
}

void author_widgets(Ui& u, ScreenState& state)
{
    using namespace string::ui;
    static const std::string_view kQuality[] = { "Low", "Medium", "High", "Ultra" };

    u.panel("widgets_panel")
        .title("Widget kit")
        .initial({ 60.0f, 100.0f, 620.0f, 440.0f })
        .limits({ 260.0f, 220.0f })
        .content([&](Ui& u2) {

            checkbox(u2, "w_wireframe", bind(state, &ScreenState::w_wireframe))
                .label("Wireframe");

            // Deliberately two checkboxes sharing a LABEL but NOT a name or a binding: names are
            // identity, labels are not. (An earlier draft bound both to the same field, so they
            // moved together and read as an identity bug — the opposite of the point.)
            checkbox(u2, "w_cull", bind(state, &ScreenState::w_cull)).label("Cull");

            slider(u2, "w_speed", bind(state, &ScreenState::w_speed))
                .label("Speed")
                .range(0.0f, 20.0f)
                .width(150)
                .precision(2);

            drag_value(u2, "w_fine", bind(state, &ScreenState::w_fine))
                .label("Fine")
                .range(0.0f, 1.0f)
                .precision(3);

            combo(u2, "w_quality", bind(state, &ScreenState::w_quality))
                .label("Quality")
                .options(kQuality);

            text_field(u2, "w_name", bind(state, &ScreenState::w_name))
                .label("Name")
                .placeholder("type here")
                .width(160);

            static const std::string_view kTabs[] = { "Basics", "Colour" };
            tabs(u2, "w_tab", bind(state, &ScreenState::w_tab))
                .options(kTabs)
                .content([&](Ui& u3) {
                    if (state.w_tab == 0)
                    {
                        u3.text("Speed jumps to where you click; Fine is a relative scrub")
                          .color(theme().text_dim)
                          .font(16);
                    }
                    else
                    {
                        color_picker(u3, "w_tint", bind(state, &ScreenState::w_tint))
                            .label("Tint")
                            .alpha(true);
                    }
                });

            // A deliberately large table: only the visible rows are built, so this costs the same
            // as a 12-row one. That is the claim virtualization makes, on screen.
            static const table_column kTableCols[] = {
                { "Index", 70 }, { "Name", 110 }, { "Value", 70 } };
            collapsible(u2, "w_table").label("Table (5000 rows)").open(false).content([&](Ui& u3) {
                table(u3, "w_tbl")
                    .columns(kTableCols)
                    .visible_rows(8)
                    .selected(bind(state, &ScreenState::w_row))
                    .on_sort([&](std::size_t c, bool asc) {
                        state.w_sort_col = c;
                        state.w_sort_asc = asc;
                    })
                    .rows(5000, [&](Ui& u4, std::size_t row, std::size_t col) {
                        const std::size_t r =
                            state.w_sort_asc ? row : (5000 - 1 - row);   // the AUTHOR sorts
                        if (col == 0) u4.text(u4.own(std::to_string(r))).font(16);
                        else if (col == 1) u4.text(u4.own("item " + std::to_string(r % 97))).font(16);
                        else u4.text(u4.own(std::to_string((r * 37) % 1000))).font(16);
                    });
            });

            // A synthetic hierarchy whose keys encode the path, so children are derivable with no
            // storage — the adapter never needs the tree to own anything.
            static const std::uint64_t kTreeRoots[] = { 1, 2, 3 };
            collapsible(u2, "w_tree").label("Tree").open(false).content([&](Ui& u3) {
                tree(u3, "w_tr")
                    .roots(kTreeRoots)
                    .children([](std::uint64_t k) { return k < 100 ? std::size_t{ 3 } : std::size_t{ 0 }; },
                             [](std::uint64_t k, std::size_t i) { return k * 10 + i + 1; })
                    .visible_rows(8)
                    .selected(bind(state, &ScreenState::w_node))
                    .nodes([&](Ui& u4, std::uint64_t key, std::size_t depth) {
                        u4.text(u4.own((depth == 0 ? "group " : depth == 1 ? "item " : "leaf ") +
                                       std::to_string(key)))
                          .font(16);
                    });
            });

            // A miniature DAG in the shape brief 14 will use: passes as nodes, dependencies as edges.
            static const std::uint64_t kGKeys[] = { 1, 2, 3, 4, 5 };
            static const graph_edge kGEdges[] = {
                { 1, 2 }, { 1, 3 }, { 2, 4 }, { 3, 4 }, { 4, 5 } };
            static const char* kGNames[] = { "depth", "gtao", "shadow", "lighting", "post" };
            collapsible(u2, "w_graph").label("Graph").open(false).content([&](Ui& u3) {
                graph(u3, "w_g")
                    .size(540, 200)
                    .node_size(90, 30)
                    .spacing(40, 14)
                    .edges(kGEdges)
                    .selected(bind(state, &ScreenState::w_gnode))
                    .nodes(kGKeys, [&](Ui& u4, std::uint64_t key) {
                        u4.text(kGNames[key - 1]).font(16);
                    });
            });

            // The general case the table and tree solve narrowly: content that is not uniform rows,
            // so it cannot be virtualized — it is all built and CLIPPED. Mixed widgets inside, to
            // show that scrolled content stays fully interactive (and stops being clickable exactly
            // where it stops being visible).
            collapsible(u2, "w_scroll").label("Scroll area").open(false).content([&](Ui& u3) {
                scroll_area(u3, "w_sc").size(300, 140).content([&](Ui& u4) {
                    u4.text("Wheel over this box, or drag the bar.")
                      .color(theme().text_dim)
                      .font(16);
                    checkbox(u4, "w_sc_a", bind(state, &ScreenState::w_wireframe)).label("Wireframe");
                    for (int i = 0; i < 14; ++i)
                        u4.text(u4.own("row " + std::to_string(i))).font(18);
                    checkbox(u4, "w_sc_b", bind(state, &ScreenState::w_cull)).label("Cull (at the end)");
                });
            });

            collapsible(u2, "w_advanced").label("Advanced").open(false).content([&](Ui& u3) {
                u3.text("a folded section emits nothing at all").color(theme().text_dim).font(16);
                checkbox(u3, "w_adv_flag", bind(state, &ScreenState::w_cull)).label("Linked to Cull");
            });
        });
}

void author_status_panel(Ui& u, const UiScene& scene, ScreenState& state,
                         std::string_view screen_name, std::size_t nameplate_count)
{
    ++state.frame;

    u.element()
        .color(theme().panel)
        .stroke(theme().stroke, 2)
        .radius(12)
        .shape(shape::ROUNDED_RECTANGLE)
        .pad(12)
        .gap(4)
        .column()
        .content([&](Ui& u2) {
            u2.text("String UI sandbox").color(theme().accent).font(26);
            u2.text(u2.own("frame " + std::to_string(state.frame))).color(theme().text).font(20);
            u2.text(u2.own("screen: " + std::string(screen_name))).color(theme().text).font(20);
            u2.text(u2.own("anchors: " + std::to_string(scene.anchors.size()) +
                           "   nameplates: " + std::to_string(nameplate_count)))
                .color(theme().text)
                .font(20);
        });
}

// ---------------------------------------------------------------------------------------------
// (2) Inventory grid + tooltip. A synthetic 6x4 item grid with rarity-coloured cells; hovering a cell
// shows a layout-sized tooltip. Drag-drop is represented by the hover/press affordance (a pressed
// cell lifts) — no live pointer drag in this synthetic bed, but the interaction plumbing is real.
void author_inventory(Ui& u)
{
    static const char* item_names[] = {
        "Sword", "Shield", "Potion", "Ring", "Boots", "Cloak",
        "Gem", "Scroll", "Bow", "Staff", "Helm", "Torch",
    };
    static const color rarities[] = {
        col::rarity_common, col::rarity_uncommon, col::rarity_rare,
        col::rarity_epic, col::rarity_legendary, col::rarity_rare,
    };

    std::uint64_t hovered_slot = 0;
    std::string hovered_item;
    color hovered_rarity = theme().text;

    u.element()
        .floating(40, 120)
        .color(theme().panel)
        .stroke(theme().stroke, 2)
        .radius(14)
        .shape(shape::ROUNDED_RECTANGLE)
        .pad(14)
        .gap(10)
        .column()
        .content([&](Ui& u2) {
            u2.text("Inventory").color(theme().text).font(24);

            // 6 columns x 4 rows grid.
            u2.element().gap(8).column().content([&](Ui& u3) {
                for (int row = 0; row < 4; ++row)
                {
                    u3.element().gap(8).row().content([&](Ui& u4) {
                        for (int cx = 0; cx < 6; ++cx)
                        {
                            const int idx = row * 6 + cx;
                            const std::string_view id_name = u4.own("inv_" + std::to_string(idx));
                            const std::string_view label =
                                u4.own(std::string(item_names[idx % 12]).substr(0, 4));
                            const auto cid = make_id(id_name).hash;
                            const color rar = rarities[idx % 6];
                            icon_cell(u4, id_name, label, rar, 56);
                            if (u4.interaction_state().is_hot(cid) ||
                                u4.interaction_state().is_focused(cid))
                            {
                                hovered_slot = cid;
                                hovered_item = item_names[idx % 12];
                                hovered_rarity = rar;
                            }
                        }
                    });
                }
            });
        });

    // Tooltip for the hovered/focused cell (floating, layout-sized).
    if (hovered_slot != 0)
    {
        // Body text must outlive the build — the arena guarantees that now; previously every author
        // had to remember it, and a local copy here rendered per-frame garbage below the title.
        static const std::string body =
            "A finely crafted item of some renown. Hover shows a layout-sized, "
            "word-wrapped tooltip body.";
        tooltip(u, 360, 140, u.own(hovered_item), hovered_rarity, &body, 1, 220);
    }
}

// ---------------------------------------------------------------------------------------------
// (3) Action bar: a hotbar of ability slots with keybind labels, radial cooldown sweeps (shader
// mask), and a proc-glow highlight (motion showcase). Clicking a ready slot triggers its cooldown.
void author_actionbar(Ui& u, ScreenState& state)
{
    static const char* keys[] = { "1", "2", "3", "4", "Q", "E", "R", "F" };
    static const char* abils[] = { "Slash", "Guard", "Heal", "Dash", "Fire", "Ice", "Ult", "Blink" };
    const std::size_t slots = 8;

    // Bottom-centred bar (floating).
    u.element()
        .floating(200, 700)
        .color(theme().panel)
        .stroke(theme().stroke, 2)
        .radius(14)
        .shape(shape::ROUNDED_RECTANGLE)
        .pad({ 12, 12, 10, 10 })
        .gap(8)
        .row()
        .content([&](Ui& u2) {
            for (std::size_t i = 0; i < slots; ++i)
            {
                const float cd_frac =
                    state.cd_total[i] > 0.0f ? state.cooldowns[i] / state.cd_total[i] : 0.0f;
                const std::string_view id_name = u2.own("act_" + std::to_string(i));
                const auto cid = make_id(id_name).hash;

                // Slot column: keybind on top, icon cell below.
                u2.element().gap(3).align(alignment::CENTER).column().content([&](Ui& u3) {
                    u3.text(keys[i]).color(theme().text_dim).font(16);

                    const std::string_view label = u3.own(std::string(abils[i]).substr(0, 3));
                    const color border = (i == 6) ? col::rarity_legendary : theme().stroke;
                    // Proc glow: pulse the "Ult" slot border when proc_glow active.
                    const color glow_border =
                        (i == 6 && state.proc_glow > 0.0f) ? theme().accent_warm : border;
                    icon_cell(u3, id_name, label, glow_border, 56, cd_frac);

                    // Activation: trigger cooldown on a ready slot (pressed edge).
                    if (u3.interaction_state().is_pressed(cid) && state.cooldowns[i] <= 0.0f)
                    {
                        state.cd_total[i] = 3.0f + static_cast<float>(i);
                        state.cooldowns[i] = state.cd_total[i];
                        if (i == 6)
                            state.proc_glow = 1.0f;
                    }
                });
            }
        });
}

// ---------------------------------------------------------------------------------------------
// (4) Quick-chat / tactical-ping radial menu + world ping markers + feed. Controller-first: the
// radial shows 6 wedges; `state.radial_sel` highlights one (advanced by gamepad d-pad / stick). The
// centre reports the selection. World ping markers float over the anchors; a feed lists recent pings.
void author_chat_pings(Ui& u, const UiScene& scene, ScreenState& state)
{
    static const char* wedges[] = { "Attack", "Defend", "On My Way", "Retreat", "Missing", "Help!" };
    const int wedge_count = 6;

    // Radial menu centred on screen (always shown in this synthetic bed for verification).
    const float cx = scene.screen.x * 0.5f;
    const float cy = scene.screen.y * 0.5f;
    const float radius = 120.0f;

    // Centre hub.
    u.element()
        .floating(static_cast<uint16_t>(cx - 34), static_cast<uint16_t>(cy - 34))
        .color(theme().panel)
        .stroke(theme().stroke_hi, 2)
        .shape(shape::CIRCLE)
        .fixed(68, 68);

    for (int i = 0; i < wedge_count; ++i)
    {
        const float ang = -1.5707963f + (6.2831853f * i) / wedge_count;
        const float wx = cx + std::cos(ang) * radius - 46;
        const float wy = cy + std::sin(ang) * radius - 24;
        const bool sel = (i == state.radial_sel);

        u.element()
            .floating(static_cast<uint16_t>(std::max(0.0f, wx)),
                      static_cast<uint16_t>(std::max(0.0f, wy)))
            .color(sel ? color{ 45, 52, 78, 240 } : theme().panel_alt)
            .stroke(sel ? theme().accent_warm : theme().stroke, sel ? 3 : 2)
            .radius(10)
            .shape(shape::ROUNDED_RECTANGLE)
            .fixed(92, 40)
            .pad({ 8, 8, 9, 9 })
            .align(alignment::CENTER)
            .row()
            .justify(justification::CENTER)
            .content([&](Ui& u2) {
                u2.text(wedges[i]).color(sel ? theme().text : theme().text_dim).font(18);
            });
    }

    // World ping markers on the first few anchors (synthetic, world-anchored).
    for (std::size_t i = 0; i < scene.anchors.size() && i < 4; ++i)
    {
        glm::vec2 px;
        float depth;
        if (!scene.project(scene.anchors[i].world, px, depth))
            continue;
        u.element()
            .floating(static_cast<uint16_t>(std::max(0.0f, px.x - 10)),
                      static_cast<uint16_t>(std::max(0.0f, px.y - 10)))
            .color(theme().accent_warm)
            .stroke(theme().bad, 2)
            .shape(shape::CIRCLE)
            .fixed(20, 20);
    }

    // Ping feed (top-right).
    u.element()
        .floating(static_cast<uint16_t>(std::max(0.0f, scene.screen.x - 240)), 120)
        .color(theme().panel)
        .stroke(theme().stroke, 2)
        .radius(10)
        .shape(shape::ROUNDED_RECTANGLE)
        .size({ fixed(220), fit() })
        .pad({ 10, 10, 8, 8 })
        .gap(4)
        .column()
        .content([&](Ui& u2) {
            u2.text("Ping feed").color(theme().accent).font(20);
            const char* sample[] = { "Aeloria: On My Way", "Bjorn: Missing", "You: Help!" };
            for (const char* s : sample)
                u2.text(s).color(theme().text_dim).size({ fixed(200), fit() }).font(16);
        });
}

// ---------------------------------------------------------------------------------------------
// ScreenState timers.
void ScreenState::tick(float dt)
{
    time += dt;
    for (std::size_t i = 0; i < cooldowns.size(); ++i)
        cooldowns[i] = std::max(0.0f, cooldowns[i] - dt);
    proc_glow = std::max(0.0f, proc_glow - dt * 1.5f);
    for (Ping& p : pings)
        p.age += dt;
    pings.erase(std::remove_if(pings.begin(), pings.end(),
                               [](const Ping& p) { return p.age > 6.0f; }),
                pings.end());
}

}  // namespace string::client
