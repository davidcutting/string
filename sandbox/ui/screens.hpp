#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <string/core/layout.hpp>

#include "passes/ui_pass.hpp"
#include "passes/ui_scene.hpp"
#include "ui/motion.hpp"
#include "ui/widgets.hpp"

namespace sandbox::ui
{

// String scratch a screen author needs to keep alive across the frame (layout text_runs are
// non-owning views, so the strings must outlive layout + record). Held in the author closure.
// A deque, NOT a vector: push_back must never relocate earlier strings — SSO string data lives
// inside the string object, so a vector regrowth moves it and dangles every view taken so far.
struct ScreenScratch
{
    std::deque<std::string> lines;
    std::uint64_t frame = 0;
};

// Persistent per-screen animation state (cooldown timers, proc glow, ping feed) — lives in the author
// closure and ticks each frame. Separate from ScreenScratch (which is per-frame string storage).
struct ScreenState
{
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
void author_nameplates(string::layout_builder& b, const UiScene& scene, ScreenScratch& scratch,
                       std::size_t budget);

// (2) Inventory grid + hover tooltip (rarity colours, icon cells, drag-drop affordance).
void author_inventory(string::layout_builder& b, Motion& m, const Interaction& it,
                      ScreenScratch& scratch);

// (3) Action bar + radial cooldown sweeps + keybind labels + proc glow (transition showcase).
void author_actionbar(string::layout_builder& b, Motion& m, const Interaction& it,
                      ScreenState& state, ScreenScratch& scratch);

// (4) Quick-chat / tactical-ping radial menu (mouse + gamepad) + ping feed + world ping markers.
void author_chat_pings(string::layout_builder& b, Motion& m, const Interaction& it,
                       const UiScene& scene, ScreenState& state, ScreenScratch& scratch);

// A small always-on status panel (frame counter, anchor/nameplate count, active screen).
void author_status_panel(string::layout_builder& b, const UiScene& scene, ScreenScratch& scratch,
                         std::string_view screen_name, std::size_t nameplate_count);

}  // namespace sandbox::ui
