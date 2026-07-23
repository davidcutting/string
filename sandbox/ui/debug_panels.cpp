#include "ui/debug_panels.hpp"

#include <algorithm>
#include <cstdio>

#include <string/debug_draw.hpp>
#include <string/platform/input.hpp>
#include <string/vulkan/gpu_profiler.hpp>

#include "debug_cvars.hpp"
#include "ui/theme.hpp"

namespace sandbox::ui
{
using string::element;
using string::format;
using string::shape;
using string::direction;
using string::make_id;
using string::size_fit;
using string::size_fixed;
using string::fit;
using string::fixed;
using string::grow;

namespace
{
// A short formatted line kept in the scratch deque so its view survives layout+record.
const std::string& stash(ScreenScratch& s, std::string line)
{
    s.lines.push_back(std::move(line));
    return s.lines.back();
}

string::color line_color(ConsoleLine::Kind k)
{
    switch (k)
    {
        case ConsoleLine::Kind::Ok:    return col::good;
        case ConsoleLine::Kind::Warn:  return col::accent_warm;
        case ConsoleLine::Kind::Error: return col::bad;
        case ConsoleLine::Kind::Echo:  return col::accent;
        default:                       return col::text;
    }
}

string::color severity_color(String::LogLevel lvl)
{
    switch (lvl)
    {
        case String::LogLevel::WARN:     return col::accent_warm;
        case String::LogLevel::ERROR:
        case String::LogLevel::CRITICAL: return col::bad;
        case String::LogLevel::DEBUG:
        case String::LogLevel::TRACE:    return col::text_dim;
        default:                         return col::text;
    }
}
}  // namespace

DebugPanels::DebugPanels() = default;

void DebugPanels::update_and_author(string::layout_builder& b, const UIPass::UiContext& ctx,
                                    const MeshOverlayStats* mesh_stats)
{
    ++scratch_.frame;
    scratch_.lines.clear();
    motion_.begin_frame(ctx.delta_time);

    handle_toggles(ctx);

    // Modal: while the console is open, suppress gameplay input (camera/debug keys route through
    // InputMap, which honours text_capture). The console reads RAW input directly, so it still types.
    ctx.input.set_text_capture(console_open_);

    if (console_open_) handle_console_input(ctx);

    if (console_open_) author_console(b);
    if (cv_logs_enabled().get()) author_logs(b);
    if (cv_hud_enabled().get()) author_hud(b, mesh_stats);
    if (cv_inspector_enabled().get()) author_inspector(b, ctx, mesh_stats);

    // Debug-draw self-test pattern (verifies the immediate-mode line pipeline end to end): RGB axes,
    // a wireframe AABB, and a wireframe sphere near the world origin, plus overlay variants above.
    if (cv_draw_test().get())
    {
        string::debug::axes(glm::mat4(1.0f), 2.0f);
        string::debug::aabb({ -1, 0, -1 }, { 1, 2, 1 }, { 0, 255, 0, 255 });
        string::debug::sphere({ 0, 3, 0 }, 1.0f, { 60, 160, 255, 255 });
        string::debug::aabb_overlay({ 1.5f, 0, -1 }, { 3.5f, 2, 1 }, { 255, 0, 255, 255 });
        string::debug::sphere_overlay({ 2.5f, 3, 0 }, 1.0f, { 255, 200, 0, 255 });
    }

    motion_.end_frame();
}

void DebugPanels::handle_toggles(const UIPass::UiContext& ctx)
{
    const String::Input& in = ctx.input;
    // The console open state is mirrored in dbg.console so a headless capture (STRING_CONSOLE=1) or a
    // console command can drive it; the grave key toggles it at runtime. Reconcile both directions.
    if (cv_console_open().get() != console_open_) console_open_ = cv_console_open().get();
    // Grave/backtick toggles the console (raw key, not InputMap, so it works while text_capture on).
    const bool grave = in.key_down(String::KeyCode::GRAVE_ACCENT);
    if (grave && !prev_grave_) console_open_ = !console_open_;
    prev_grave_ = grave;
    cv_console_open().set(console_open_);

    // F2 -> HUD, F3 -> inspector, driving the CVars so console `dbg.hud`/`dbg.inspector` stay in sync.
    const bool f2 = in.key_down(String::KeyCode::F2);
    if (f2 && !prev_f2_) cv_hud_enabled().set(!cv_hud_enabled().get());
    prev_f2_ = f2;
    const bool f3 = in.key_down(String::KeyCode::F3);
    if (f3 && !prev_f3_) cv_inspector_enabled().set(!cv_inspector_enabled().get());
    prev_f3_ = f3;
    const bool f4 = in.key_down(String::KeyCode::F4);
    if (f4 && !prev_f4_) cv_logs_enabled().set(!cv_logs_enabled().get());
    prev_f4_ = f4;
}

void DebugPanels::handle_console_input(const UIPass::UiContext& ctx)
{
    const String::Input& in = ctx.input;

    // Composed character input (honours layout/shift). Filter out the toggling backtick so opening
    // the console never leaves a stray '`' in the edit line.
    for (char c : in.typed_text())
        if (c != '`' && c != '~') input_ += c;

    if (in.key_pressed(String::KeyCode::BACKSPACE) && !input_.empty()) input_.pop_back();

    const bool enter = in.key_down(String::KeyCode::ENTER);
    if (enter && !prev_enter_ && !input_.empty())
    {
        console_.execute(input_);
        input_.clear();
    }
    prev_enter_ = enter;

    const bool tab = in.key_down(String::KeyCode::TAB);
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

    const bool up = in.key_down(String::KeyCode::UP);
    if (up && !prev_up_) { std::string h = console_.history_prev(); if (!h.empty()) input_ = h; }
    prev_up_ = up;
    const bool down = in.key_down(String::KeyCode::DOWN);
    if (down && !prev_down_) input_ = console_.history_next();
    prev_down_ = down;
}

void DebugPanels::author_console(string::layout_builder& b)
{
    element panel{};
    panel.id = make_id("dbg_console");
    panel.floating = true;
    panel.overlay = true;   // topmost layer: debug surfaces cover scene UI text AND shapes
    panel.float_x = 16;
    panel.float_y = 16;
    // Fully opaque: the console overlays arbitrary scene/UI content and must stay readable.
    panel.color = { col::panel_alt.r, col::panel_alt.g, col::panel_alt.b, 255 };
    panel.stroke_color = col::stroke_hi;
    panel.stroke_width = 2;
    panel.radius = 10;
    panel.shape = shape::ROUNDED_RECTANGLE;
    panel.sizing = { fixed(760), fit() };

    b.begin(panel, format{ .padding = { 10, 10, 10, 10 }, .gap = 3, .direction = direction::VERTICAL });

    element title{};
    title.color = col::accent;
    title.sizing = size_fit();
    b.add_text(title, stash(scratch_, "CONSOLE  (` close · Tab complete · Up/Down history · Enter run · F4 logs)"), 18);

    // Console command output only — the LOG TAIL is its own panel (F4/dbg.logs). Mixing streams
    // flooded the console and per-frame log arrivals resized/jittered the whole surface (user).
    // Command output changes only when a command runs, so fit-height is stable here.
    const std::deque<ConsoleLine>& out = console_.output();
    const std::size_t show = std::min<std::size_t>(10, out.size());
    for (std::size_t i = out.size() - show; i < out.size(); ++i)
    {
        element row{};
        row.color = line_color(out[i].kind);
        row.sizing = { fixed(740), fit() };
        b.add_text(row, stash(scratch_, out[i].text), 15);
    }

    // Edit line with a blinking-ish caret.
    element edit{};
    edit.id = make_id("dbg_console_edit");
    edit.color = col::accent_warm;
    edit.sizing = { fixed(740), fit() };
    b.add_text(edit, stash(scratch_, "> " + input_ + "_"), 18);

    b.end();
}

// Log tail panel (F4 / dbg.logs) — deliberately SEPARATE from the console (user: reading logs and
// entering commands are different tasks; mixing them floods the console). Anti-jitter design: a
// FIXED number of single-line rows, newest FIRST, each row grow-width (single line, clipped at the
// panel edge — the wrap contract only wraps fixed-width elements). Row count and row height never
// change as logs arrive, so the panel geometry is rock-stable; long lines truncate (full text is in
// the terminal/log file).
void DebugPanels::author_logs(string::layout_builder& b)
{
    element panel{};
    panel.id = make_id("dbg_logs");
    panel.floating = true;
    panel.overlay = true;   // topmost layer: debug surfaces cover scene UI text AND shapes
    panel.float_x = 16;
    panel.float_y = console_open_ ? 400 : 16;   // stacks under the console when both are open
    panel.color = { col::panel_alt.r, col::panel_alt.g, col::panel_alt.b, 255 };
    panel.stroke_color = col::stroke_hi;
    panel.stroke_width = 2;
    panel.radius = 10;
    panel.shape = shape::ROUNDED_RECTANGLE;
    panel.sizing = { fixed(760), fit() };

    b.begin(panel, format{ .padding = { 10, 10, 10, 10 }, .gap = 3, .direction = direction::VERTICAL });

    element title{};
    title.color = col::accent;
    title.sizing = size_fit();
    b.add_text(title, stash(scratch_, "LOGS  (newest first · F4 close)"), 18);

    constexpr std::size_t kLogRows = 14;
    const std::vector<String::LogRingBuffer::Line> log =
        String::LogRingBuffer::instance().tail(kLogRows);
    for (std::size_t i = 0; i < kLogRows; ++i)
    {
        element row{};
        // Newest first: index from the back of the tail. Missing rows render as empty lines so the
        // panel height is constant from the first frame.
        const bool have = i < log.size();
        const auto& ln = log[have ? log.size() - 1 - i : 0];
        row.color = have ? severity_color(ln.level) : col::text_dim;
        row.sizing = { grow(0, 740), fit() };   // grow (not fixed) width: single line, clipped
        b.add_text(row, stash(scratch_, have ? ln.text : std::string{ " " }), 15);
    }
    b.end();
}

void DebugPanels::author_hud(string::layout_builder& b, const MeshOverlayStats* mesh_stats)
{
    element panel{};
    panel.id = make_id("dbg_hud");
    panel.floating = true;
    panel.overlay = true;   // topmost layer: debug surfaces cover scene UI text AND shapes
    // Top-left column stacking: below the console and/or log panel when they're open.
    panel.float_x = 16;
    panel.float_y = static_cast<uint16_t>(
        16 + (console_open_ ? 384 : 0) + (cv_logs_enabled().get() ? 384 : 0) +
        (!console_open_ && !cv_logs_enabled().get() ? 174 : 0));
    panel.color = col::panel;
    panel.stroke_color = col::stroke;
    panel.stroke_width = 2;
    panel.radius = 10;
    panel.shape = shape::ROUNDED_RECTANGLE;
    panel.sizing = { fixed(300), fit() };

    b.begin(panel, format{ .padding = { 10, 10, 10, 10 }, .gap = 3, .direction = direction::VERTICAL });

    element title{};
    title.color = col::accent;
    title.sizing = size_fit();
    b.add_text(title, stash(scratch_, "PROFILER HUD"), 18);

    // Per-pass GPU ms from the engine's always-on timestamp readback (global handle).
    const String::GpuProfiler* prof = String::GpuProfiler::global();
    char buf[96];
    if (prof && prof->enabled())
    {
        const std::vector<String::GpuProfiler::PassStat> ps = prof->stats();
        for (const auto& s : ps)
        {
            element row{};
            row.color = col::text;
            row.sizing = { fixed(280), fit() };
            std::snprintf(buf, sizeof(buf), "%-16s %6.3f ms", s.name.c_str(), s.avg_ms);
            b.add_text(row, stash(scratch_, buf), 15);
        }
        element total{};
        total.color = col::good;
        total.sizing = size_fit();
        std::snprintf(buf, sizeof(buf), "GPU total       %6.3f ms", prof->total_avg_ms());
        b.add_text(total, stash(scratch_, buf), 16);
    }
    else
    {
        element row{};
        row.color = col::text_dim;
        row.sizing = size_fit();
        b.add_text(row, stash(scratch_, "gpu timing unavailable"), 15);
    }

    // Consolidated mesh/cull stats (formerly the standalone overlay in demo_scene.cpp): ONE stats
    // surface. Kept informative — the same numbers agents used to grep from the overlay.
    if (mesh_stats)
    {
        const GpuMeshStats& s = mesh_stats->stats;
        static const char* view_names[] = { "off", "meshlet", "LOD", "occlusion" };
        auto pct = [](uint32_t num, uint32_t den) {
            return den == 0 ? std::string("--") : std::to_string(num * 100 / den) + "%";
        };
        const auto add = [&](std::string t) {
            element row{};
            row.color = col::text_dim;
            row.sizing = { fixed(280), fit() };
            b.add_text(row, stash(scratch_, std::move(t)), 15);
        };
        add("HiZ " + std::string(mesh_stats->hiz_enabled ? "on" : "off")
            + "  view " + view_names[mesh_stats->debug_view & 3]
            + (mesh_stats->crowd_enabled ? "  [CROWD]" : ""));
        add("meshlets " + std::to_string(s.meshlets_total) + " / "
            + std::to_string(mesh_stats->total_meshlets));
        add("frustum " + std::to_string(s.after_frustum) + " (" + pct(s.after_frustum, s.meshlets_total) + ")");
        add("cone    " + std::to_string(s.after_cone) + " (" + pct(s.after_cone, s.meshlets_total) + ")");
        add("drawn   " + std::to_string(s.after_hiz) + " (" + pct(s.after_hiz, s.meshlets_total) + ")");
        add("draws " + std::to_string(mesh_stats->draw_count) + "  lights "
            + std::to_string(mesh_stats->lights.size()));
    }

    b.end();
}

void DebugPanels::author_inspector(string::layout_builder& b, const UIPass::UiContext& ctx,
                                   const MeshOverlayStats* mesh_stats)
{
    element panel{};
    panel.id = make_id("dbg_inspector");
    panel.floating = true;
    panel.overlay = true;   // topmost layer: debug surfaces cover scene UI text AND shapes
    panel.float_x = 340;
    panel.float_y = 16;
    panel.color = col::panel;
    panel.stroke_color = col::stroke;
    panel.stroke_width = 2;
    panel.radius = 10;
    panel.shape = shape::ROUNDED_RECTANGLE;
    panel.sizing = { fixed(360), fit() };

    b.begin(panel, format{ .padding = { 10, 10, 10, 10 }, .gap = 2, .direction = direction::VERTICAL });

    element title{};
    title.color = col::accent;
    title.sizing = size_fit();
    b.add_text(title, stash(scratch_, "SCENE / DRAW INSPECTOR"), 18);

    if (!mesh_stats || mesh_stats->draws.empty())
    {
        element row{};
        row.color = col::text_dim;
        row.sizing = size_fit();
        b.add_text(row, stash(scratch_, "no scene loaded"), 15);
        b.end();
        return;
    }

    const int isolate = cv_isolate_draw().get();
    element hdr{};
    hdr.color = col::text_dim;
    hdr.sizing = size_fit();
    b.add_text(hdr, stash(scratch_, "idx  meshlets lod res   (hover to highlight, isolate="
                                    + std::to_string(isolate) + ")"), 14);

    // Scrollable window of rows. Scroll follows the hovered row via PAGE_UP/DOWN; a simple fixed
    // window keeps v1 read-only and cheap. Hovered row -> AABB highlight via debug-draw.
    constexpr int kRows = 18;
    const int total = int(mesh_stats->draws.size());
    if (ctx.input.key_pressed(String::KeyCode::PAGE_DOWN)) draw_scroll_ += kRows;
    if (ctx.input.key_pressed(String::KeyCode::PAGE_UP)) draw_scroll_ -= kRows;
    draw_scroll_ = std::clamp(draw_scroll_, 0, std::max(0, total - kRows));

    for (int r = draw_scroll_; r < std::min(total, draw_scroll_ + kRows); ++r)
    {
        const InspectorDraw& d = mesh_stats->draws[r];
        const std::string row_id = "dbg_draw_" + std::to_string(r);
        element row{};
        row.id = make_id(stash(scratch_, row_id).c_str());  // id name must outlive layout (scratch)
        const bool hot = ctx.hovered == row.id.hash;
        const bool sel = r == selected_draw_ || r == isolate;
        row.color = sel ? col::accent : (hot ? col::accent_warm : col::text);
        row.sizing = { fixed(340), fit() };
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%-4d %8u %3u %s",
                      d.index, d.meshlet_count, d.lod_count, d.resident ? "Y" : "-");
        b.add_text(row, stash(scratch_, buf), 14);

        // Hover/select highlight: draw the draw's AABB in-world via the debug-draw API.
        if (hot || sel)
        {
            string::debug::aabb(d.aabb_min, d.aabb_max,
                                sel ? string::debug::Color{ 137, 180, 250, 255 }
                                    : string::debug::Color{ 249, 226, 175, 255 });
            if (hot) selected_draw_ = r;
        }
    }

    // Lights list header + a few rows.
    element lhdr{};
    lhdr.color = col::text_dim;
    lhdr.sizing = size_fit();
    b.add_text(lhdr, stash(scratch_, "lights: " + std::to_string(mesh_stats->lights.size())), 14);
    const int lmax = std::min<int>(6, int(mesh_stats->lights.size()));
    for (int i = 0; i < lmax; ++i)
    {
        const InspectorLight& L = mesh_stats->lights[i];
        element row{};
        row.color = col::text;
        row.sizing = { fixed(340), fit() };
        char buf[128];
        std::snprintf(buf, sizeof(buf), "L%-2d (%.1f,%.1f,%.1f) r=%.1f %s",
                      i, L.position.x, L.position.y, L.position.z, L.range, L.spot ? "spot" : "point");
        b.add_text(row, stash(scratch_, buf), 13);
        // Mark each light with a small overlay sphere so it's locatable in-world.
        string::debug::sphere_overlay(L.position, 0.15f, string::debug::Color{ 249, 226, 175, 255 }, 10);
    }

    b.end();
}

}  // namespace sandbox::ui
