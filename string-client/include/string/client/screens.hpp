#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <string/ui/layout.hpp>

#include <string/client/ui_scene.hpp>
#include <string/ui/motion.hpp>
#include <string/client/widgets.hpp>

namespace string::client
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
    // Brief 13 M0a widget demo state.
    bool w_wireframe = false;
    bool w_cull = false;
    float w_fine = 0.5f;
    std::string w_name = "Sponza";
    int w_tab = 0;
    int w_row = -1;
    std::size_t w_sort_col = 0;
    std::uint64_t w_node = 0;
    std::uint64_t w_gnode = 0;
    bool w_sort_asc = true;
    ::string::color w_tint{ 137, 180, 250, 255 };
    float w_speed = 5.0f;
    int w_quality = 1;

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

// (5) Brief 12 M1: floating panels — drag by the title bar, resize by the corner grip.
// Deliberately its OWN screen and NOT part of "all", so the five brief-05 layout-dump baselines
// stay byte-identical through M1.
void author_panels(Ui& u, ScreenState& state);

// (6) Brief 12 M2c: the workspace — cards arranged into split regions with tab bars. Drag a
// splitter to resize; click a tab to switch card. Its own screen so the five brief-05 baselines
// stay byte-identical.
//
// Card identities are CONSTANTS, so a typo is a compile error rather than a card that silently
// never appears. `make_id` is constexpr, so these cost nothing at runtime.
inline constexpr ::string::id kCardStats = ::string::make_id("ws_stats");
inline constexpr ::string::id kCardTree  = ::string::make_id("ws_tree");
inline constexpr ::string::id kCardLog   = ::string::make_id("ws_log");
inline constexpr ::string::id kCardProps = ::string::make_id("ws_props");

// Seeds the default arrangement. A SEED, not a static relationship: in a real app this runs only
// when there is nothing saved, and the user owns the layout from then on.
void seed_workspace(::string::ui::Workspace& ws);
void author_workspace(Ui& u, ::string::ui::Workspace& ws, ScreenState& state);

// (7) Brief 13 M0a: the engine widget kit — checkbox, drag-value, combo.
void author_widgets(Ui& u, ScreenState& state);

// A small always-on status panel (frame counter, anchor/nameplate count, active screen).
void author_status_panel(Ui& u, const UiScene& scene, ScreenState& state,
                         std::string_view screen_name, std::size_t nameplate_count);

}  // namespace string::client
