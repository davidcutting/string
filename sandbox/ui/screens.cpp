#include "ui/screens.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "ui/theme.hpp"

namespace sandbox::ui
{
using namespace string;

namespace
{

// A horizontal bar (rounded) with a proportional fill child, as a floating overlay subtree at (x,y).
// width/height in px; `frac` 0..1. Colours pre-faded by the caller. Returns nothing (adds to b).
void bar(layout_builder& b, uint16_t x, uint16_t y, uint16_t w, uint16_t h, float frac,
         color bg, color fill)
{
    frac = std::clamp(frac, 0.0f, 1.0f);
    element track{};
    track.floating = true;
    track.float_x = x;
    track.float_y = y;
    track.color = bg;
    track.radius = h / 2;
    track.shape = shape::ROUNDED_RECTANGLE;
    track.sizing = size_fixed(w, h);

    element fill_el{};
    fill_el.color = fill;
    fill_el.radius = h / 2;
    fill_el.shape = shape::ROUNDED_RECTANGLE;
    const uint16_t fw = static_cast<uint16_t>(std::max(1.0f, frac * w));
    fill_el.sizing = size_fixed(fw, h);

    b.begin(track, format{ .direction = direction::HORIZONTAL })
         .add_element(fill_el)
     .end();
}

color fade(color c, float a)
{
    return color{ c.r, c.g, c.b, static_cast<uint8_t>(std::clamp(c.a * a, 0.0f, 255.0f)) };
}

}  // namespace

void author_nameplates(layout_builder& b, const UiScene& scene, ScreenScratch& scratch,
                       std::size_t budget)
{
    scratch.lines.clear();
    // scratch.lines is a deque: push_back never relocates earlier strings, so views stay valid
    // for the whole frame without reserve gymnastics.
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

        // Name label (floating text), colour by faction, pre-faded.
        scratch.lines.push_back(a.name);
        element name{};
        name.floating = true;
        name.float_x = ix;
        name.float_y = iy;
        name.sizing = size_fit();
        name.color = fade(a.hostile ? col::hostile : col::friendly, alpha);
        b.add_text(name, scratch.lines.back(), font_px);

        // Health bar just below the name.
        bar(b, ix, static_cast<uint16_t>(iy + font_px + 2), bar_w, 8, a.health,
            fade(col::hp_bg, alpha), fade(col::hp_fill, alpha));

        // Cast bar (only while casting) below the health bar.
        if (a.cast >= 0.0f)
        {
            bar(b, ix, static_cast<uint16_t>(iy + font_px + 13), bar_w, 6, a.cast,
                fade(col::hp_bg, alpha), fade(col::cast_fill, alpha));
        }
    }
}

void author_status_panel(layout_builder& b, const UiScene& scene, ScreenScratch& scratch,
                         std::string_view screen_name, std::size_t nameplate_count)
{
    ++scratch.frame;

    element panel{};
    panel.color = col::panel;
    panel.stroke_color = col::stroke;
    panel.stroke_width = 2;
    panel.radius = 12;
    panel.shape = shape::ROUNDED_RECTANGLE;
    panel.sizing = size_fit();

    // The status strings live in a small dedicated tail of scratch.lines (after any nameplate names);
    // push them and hold the views (deque storage — appends never invalidate earlier views).
    const std::size_t base = scratch.lines.size();
    scratch.lines.push_back("String UI sandbox");
    scratch.lines.push_back("frame " + std::to_string(scratch.frame));
    scratch.lines.push_back("screen: " + std::string(screen_name));
    scratch.lines.push_back("anchors: " + std::to_string(scene.anchors.size()) +
                            "   nameplates: " + std::to_string(nameplate_count));

    b.begin(panel, format{ .padding = { 12, 12, 12, 12 }, .gap = 4, .direction = direction::VERTICAL });
    for (std::size_t i = base; i < scratch.lines.size(); ++i)
    {
        element row{};
        row.sizing = size_fit();
        row.color = (i == base) ? col::accent : col::text;
        b.add_text(row, scratch.lines[i], (i == base) ? 26 : 20);
    }
    b.end();
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

// ---------------------------------------------------------------------------------------------
// (2) Inventory grid + tooltip. A synthetic 6x4 item grid with rarity-coloured cells; hovering a cell
// shows a layout-sized tooltip. Drag-drop is represented by the hover/press affordance (a pressed
// cell lifts) — no live pointer drag in this synthetic bed, but the interaction plumbing is real.
void author_inventory(layout_builder& b, Motion& m, const Interaction& it, ScreenScratch& scratch)
{
    static const char* item_names[] = {
        "Sword", "Shield", "Potion", "Ring", "Boots", "Cloak",
        "Gem", "Scroll", "Bow", "Staff", "Helm", "Torch",
    };
    static const color rarities[] = {
        col::rarity_common, col::rarity_uncommon, col::rarity_rare,
        col::rarity_epic, col::rarity_legendary, col::rarity_rare,
    };
    const std::size_t base = scratch.lines.size();

    element root{};
    root.floating = true;
    root.float_x = 40;
    root.float_y = 120;
    root.color = col::panel;
    root.stroke_color = col::stroke;
    root.stroke_width = 2;
    root.radius = 14;
    root.shape = shape::ROUNDED_RECTANGLE;
    root.sizing = size_fit();
    b.begin(root, format{ .padding = { 14, 14, 14, 14 }, .gap = 10, .direction = direction::VERTICAL });

    element header{};
    header.color = col::text;
    header.sizing = size_fit();
    scratch.lines.push_back("Inventory");
    b.add_text(header, scratch.lines.back(), 24);

    // 6 columns x 4 rows grid.
    std::uint64_t hovered_slot = 0;
    std::string hovered_item;
    color hovered_rarity = col::text;
    b.begin(format{ .gap = 8, .direction = direction::VERTICAL });
    for (int row = 0; row < 4; ++row)
    {
        b.begin(format{ .gap = 8, .direction = direction::HORIZONTAL });
        for (int cx = 0; cx < 6; ++cx)
        {
            const int idx = row * 6 + cx;
            scratch.lines.push_back("inv_" + std::to_string(idx));   // stable id string
            const std::string& id_name = scratch.lines.back();
            scratch.lines.push_back(std::string(item_names[idx % 12]).substr(0, 4));  // cell label
            const std::string& label = scratch.lines.back();
            const auto cid = make_id(id_name).hash;
            const color rar = rarities[idx % 6];
            icon_cell(b, m, it, id_name, label, rar, 56);
            if (it.is_hot(cid) || it.is_focused(cid))
            {
                hovered_slot = cid;
                hovered_item = item_names[idx % 12];
                hovered_rarity = rar;
            }
        }
        b.end();
    }
    b.end();
    b.end();

    // Tooltip for the hovered/focused cell (floating, layout-sized).
    if (hovered_slot != 0)
    {
        scratch.lines.push_back(hovered_item);
        const std::string& title = scratch.lines.back();
        // Body text must live in scratch too — a local copy dies before the pack stage reads the
        // layout's text views (this rendered per-frame garbage below the title).
        scratch.lines.push_back("A finely crafted item of some renown. Hover shows a layout-sized, "
                                "word-wrapped tooltip body.");
        tooltip(b, 360, 140, title, hovered_rarity, &scratch.lines.back(), 1, 220);
    }
}

// ---------------------------------------------------------------------------------------------
// (3) Action bar: a hotbar of ability slots with keybind labels, radial cooldown sweeps (shader
// mask), and a proc-glow highlight (motion showcase). Clicking a ready slot triggers its cooldown.
void author_actionbar(layout_builder& b, Motion& m, const Interaction& it, ScreenState& state,
                      ScreenScratch& scratch)
{
    static const char* keys[] = { "1", "2", "3", "4", "Q", "E", "R", "F" };
    static const char* abils[] = { "Slash", "Guard", "Heal", "Dash", "Fire", "Ice", "Ult", "Blink" };
    const std::size_t slots = 8;

    // Bottom-centred bar (floating).
    element root{};
    root.floating = true;
    root.float_x = 200;
    root.float_y = 700;
    root.color = col::panel;
    root.stroke_color = col::stroke;
    root.stroke_width = 2;
    root.radius = 14;
    root.shape = shape::ROUNDED_RECTANGLE;
    root.sizing = size_fit();
    b.begin(root, format{ .padding = { 12, 12, 10, 10 }, .gap = 8, .direction = direction::HORIZONTAL });

    for (std::size_t i = 0; i < slots; ++i)
    {
        const float cd_frac = state.cd_total[i] > 0.0f ? state.cooldowns[i] / state.cd_total[i] : 0.0f;
        scratch.lines.push_back("act_" + std::to_string(i));   // stable id string
        const std::string& id_name = scratch.lines.back();
        const auto cid = make_id(id_name).hash;

        // Slot column: keybind on top, icon cell below.
        b.begin(format{ .gap = 3, .alignment = alignment::CENTER, .direction = direction::VERTICAL });
        element key{};
        key.color = col::text_dim;
        key.sizing = size_fit();
        scratch.lines.push_back(keys[i]);
        b.add_text(key, scratch.lines.back(), 16);

        scratch.lines.push_back(std::string(abils[i]).substr(0, 3));  // 3-char ability tag
        const std::string& label = scratch.lines.back();
        const color border = (i == 6) ? col::rarity_legendary : col::stroke;  // "Ult" pops
        // Proc glow: pulse the "Ult" slot border when proc_glow active.
        const color glow_border = (i == 6 && state.proc_glow > 0.0f) ? col::accent_warm : border;
        icon_cell(b, m, it, id_name, label, glow_border, 56, cd_frac);

        // Activation: trigger cooldown on a ready slot (pressed edge).
        if (it.is_pressed(cid) && state.cooldowns[i] <= 0.0f)
        {
            state.cd_total[i] = 3.0f + static_cast<float>(i);
            state.cooldowns[i] = state.cd_total[i];
            if (i == 6)
                state.proc_glow = 1.0f;
        }
        b.end();
    }
    b.end();
}

// ---------------------------------------------------------------------------------------------
// (4) Quick-chat / tactical-ping radial menu + world ping markers + feed. Controller-first: the
// radial shows 6 wedges; `state.radial_sel` highlights one (advanced by gamepad d-pad / stick). The
// centre reports the selection. World ping markers float over the anchors; a feed lists recent pings.
void author_chat_pings(layout_builder& b, Motion& m, const Interaction& it, const UiScene& scene,
                       ScreenState& state, ScreenScratch& scratch)
{
    (void)m;
    (void)it;
    static const char* wedges[] = { "Attack", "Defend", "On My Way", "Retreat", "Missing", "Help!" };
    const int wedge_count = 6;

    // Radial menu centred on screen (always shown in this synthetic bed for verification).
    const float cx = scene.screen.x * 0.5f;
    const float cy = scene.screen.y * 0.5f;
    const float radius = 120.0f;
    // Centre hub.
    element hub{};
    hub.floating = true;
    hub.float_x = static_cast<uint16_t>(cx - 34);
    hub.float_y = static_cast<uint16_t>(cy - 34);
    hub.color = col::panel;
    hub.stroke_color = col::stroke_hi;
    hub.stroke_width = 2;
    hub.shape = shape::CIRCLE;
    hub.sizing = size_fixed(68, 68);
    b.add_element(hub);

    for (int i = 0; i < wedge_count; ++i)
    {
        const float ang = -1.5707963f + (6.2831853f * i) / wedge_count;
        const float wx = cx + std::cos(ang) * radius - 46;
        const float wy = cy + std::sin(ang) * radius - 24;
        const bool sel = (i == state.radial_sel);

        element wedge{};
        wedge.floating = true;
        wedge.float_x = static_cast<uint16_t>(std::max(0.0f, wx));
        wedge.float_y = static_cast<uint16_t>(std::max(0.0f, wy));
        wedge.color = sel ? color{ 45, 52, 78, 240 } : col::panel_alt;
        wedge.stroke_color = sel ? col::accent_warm : col::stroke;
        wedge.stroke_width = sel ? 3 : 2;
        wedge.radius = 10;
        wedge.shape = shape::ROUNDED_RECTANGLE;
        wedge.sizing = size_fixed(92, 40);
        element label{};
        label.color = sel ? col::text : col::text_dim;
        label.sizing = size_fit();
        scratch.lines.push_back(wedges[i]);
        b.begin(wedge, format{ .padding = { 8, 8, 9, 9 }, .alignment = alignment::CENTER,
                               .direction = direction::HORIZONTAL, .justify = justification::CENTER })
             .add_text(label, scratch.lines.back(), 18)
         .end();
    }

    // World ping markers on the first few anchors (synthetic, world-anchored).
    for (std::size_t i = 0; i < scene.anchors.size() && i < 4; ++i)
    {
        glm::vec2 px;
        float depth;
        if (!scene.project(scene.anchors[i].world, px, depth))
            continue;
        element marker{};
        marker.floating = true;
        marker.float_x = static_cast<uint16_t>(std::max(0.0f, px.x - 10));
        marker.float_y = static_cast<uint16_t>(std::max(0.0f, px.y - 10));
        marker.color = col::accent_warm;
        marker.stroke_color = col::bad;
        marker.stroke_width = 2;
        marker.shape = shape::CIRCLE;
        marker.sizing = size_fixed(20, 20);
        b.add_element(marker);
    }

    // Ping feed (top-right).
    element feed{};
    feed.floating = true;
    feed.float_x = static_cast<uint16_t>(std::max(0.0f, scene.screen.x - 240));
    feed.float_y = 120;
    feed.color = col::panel;
    feed.stroke_color = col::stroke;
    feed.stroke_width = 2;
    feed.radius = 10;
    feed.shape = shape::ROUNDED_RECTANGLE;
    feed.sizing = { fixed(220), fit() };
    b.begin(feed, format{ .padding = { 10, 10, 8, 8 }, .gap = 4, .direction = direction::VERTICAL });
    element ft{};
    ft.color = col::accent;
    ft.sizing = size_fit();
    scratch.lines.push_back("Ping feed");
    b.add_text(ft, scratch.lines.back(), 20);
    const char* sample[] = { "Aeloria: On My Way", "Bjorn: Missing", "You: Help!" };
    for (const char* s : sample)
    {
        element row{};
        row.color = col::text_dim;
        row.sizing = { fixed(200), fit() };
        scratch.lines.push_back(s);
        b.add_text(row, scratch.lines.back(), 16);
    }
    b.end();
}

}  // namespace sandbox::ui
