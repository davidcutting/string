#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include <string/core/layout.hpp>

#include "passes/meshlet_data.hpp"
#include "passes/ui_pass.hpp"
#include "ui/debug_console.hpp"
#include "ui/screens.hpp"

namespace sandbox::ui
{

// Brief 06 debug surfaces: console, profiler HUD, and scene/draw inspector, authored with the
// brief-05 widget/layout kit. One persistent state object lives in the UI author closure and drives
// all three each frame. The console is modal (grave/backtick toggles; while open it sets
// Input::text_capture so gameplay input is suppressed). The HUD/inspector are CVar+key toggled.
//
// Kept together because they share persistent state (open flags, the console's edit line/history)
// and one frame-persistent string scratch (PanelScratch's deque — layout text_runs are non-owning
// views, so every add_text string must live here, never a local).
// Frame-persistent string storage for the brief-06 debug surfaces. These are NOT brief-05 screens:
// briefs 14/15 rewrite the debug panels wholesale onto the fluent facade, so re-expressing them
// through it now would be work thrown away. Same deque invariant as before — push_back must never
// relocate earlier strings, because layout text_runs are non-owning views.
struct PanelScratch
{
    std::deque<std::string> lines;
    std::uint64_t frame = 0;
};

class DebugPanels
{
public:
    DebugPanels();

    // Called once per frame from the UI author. Reads input (toggles, console typing), then authors
    // whatever surfaces are open into `b`. `mesh_stats` may be null (ui-dev scene) — the inspector
    // then shows only "no scene" and the HUD still shows GPU timing.
    void update_and_author(string::layout_builder& b, const UIPass::UiContext& ctx,
                           const MeshOverlayStats* mesh_stats);

private:
    void handle_toggles(const UIPass::UiContext& ctx);
    void handle_console_input(const UIPass::UiContext& ctx);
    void author_console(string::layout_builder& b);
    void author_logs(string::layout_builder& b);
    void author_hud(string::layout_builder& b, const MeshOverlayStats* mesh_stats);
    void author_inspector(string::layout_builder& b, const UIPass::UiContext& ctx,
                          const MeshOverlayStats* mesh_stats);

    DebugConsole console_;
    bool console_open_ = false;
    std::string input_;               // current console edit line

    PanelScratch scratch_;           // frame-persistent string storage (see PanelScratch)
    Motion motion_;

    // Inspector selection/scroll state.
    int selected_draw_ = -1;
    int draw_scroll_ = 0;             // first visible row

    bool prev_grave_ = false;         // grave-key edge detection (raw input, not InputMap)
    bool prev_f2_ = false;
    bool prev_f3_ = false;
    bool prev_f4_ = false;
    bool prev_tab_ = false;
    bool prev_enter_ = false;
    bool prev_up_ = false;
    bool prev_down_ = false;
};

}  // namespace sandbox::ui
