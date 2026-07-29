#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <string/core/layout.hpp>

#include "passes/ui_pass.hpp"
#include "passes/ui_scene.hpp"
#include <string/ui/motion.hpp>
#include "ui/widgets.hpp"

namespace sandbox::ui
{

// Persistent per-screen animation state (cooldown timers, proc glow, ping feed) — lives in the
// author closure and ticks each frame.
//
// The per-frame STRING scratch that used to sit beside this (`ScreenScratch`, a deque whose
// invariant every author had to know) is gone as of brief 12 M0c: `Ui::own()` owns the frame arena,
// so a screen simply hands a temporary to `.text(...)` and the facade keeps it alive.
struct ScreenState
{
    std::uint64_t frame = 0;   // status-panel frame counter
    float time = 0.0f;
    std::array<float, 8> cooldowns{};   // action-bar slot cooldown remaining (s); 0 = ready
    std::array<float, 8> cd_total{};    // full cooldown duration per slot
    float proc_glow = 0.0f;             // 0..1 proc highlight on a bar slot
    // Ping feed: recent world-anchored pings (index into scene anchors + age).
    struct Ping { std::size_t anchor = 0; float age = 0.0f; int kind = 0; };
    std::vector<Ping> pings;
    bool radial_open = false;           // quick-chat / ping radial menu visible
    int radial_sel = 0;                 // controller-first: currently highlighted wedge

    void tick(float dt);
};

// --- Screen authors (brief 05 acceptance test) ------------------------------------------------

// (1) World-anchored nameplates + cast bars; synthetic stress up to `budget` (0 = all).
void author_nameplates(Ui& u, const UiScene& scene, std::size_t budget);

// (2) Inventory grid + hover tooltip (rarity colours, icon cells, drag-drop affordance).
void author_inventory(Ui& u);

// (3) Action bar + radial cooldown sweeps + keybind labels + proc glow (transition showcase).
void author_actionbar(Ui& u, ScreenState& state);

// (4) Quick-chat / tactical-ping radial menu (mouse + gamepad) + ping feed + world ping markers.
void author_chat_pings(Ui& u, const UiScene& scene, ScreenState& state);

// A small always-on status panel (frame counter, anchor/nameplate count, active screen).
void author_status_panel(Ui& u, const UiScene& scene, ScreenState& state,
                         std::string_view screen_name, std::size_t nameplate_count);

}  // namespace sandbox::ui
