#pragma once

#include <string>

#include <string/core/cvar.hpp>

// What is left once every library owns its own levers: the two choices that are genuinely the
// APP's. The renderer's tuning lives in <string/render/render_cvars.hpp>, the debug-surface
// toggles in <string/debug/debug_cvars.hpp>.
namespace sandbox
{

// Registers the app's CVars. Pure registration — main.cpp applies the env bridge once, after every
// library has registered.
void register_debug_cvars();

// Which scene the demo builds: "demo" = full Sponza + UI; "lookdev" = the material/lighting probe
// scene; "ui" = the UI-dev sandbox (background + UI passes only, near-instant startup).
string::core::CVar<std::string>& cv_scene();        // dbg.scene         <- STRING_SCENE
// Synthetic nameplate stress count for the ui-dev scene's nameplate screen (0 = off).
string::core::CVar<int32_t>& cv_ui_nameplates();    // dbg.ui.nameplates <- STRING_UI_NAMEPLATES

// Brief 23: the demo's animation controls (the app-side control surface — the engine samples and
// blends, WHAT plays is decided here).
// Clip to cross-fade every skinned character to. Empty = the auto demo below; an unknown name
// logs the available clips once.
string::core::CVar<std::string>& cv_anim_clip();    // anim.clip         <- STRING_ANIM_CLIP
// Cross-fade duration in seconds (0 = snap).
string::core::CVar<float>& cv_anim_blend();         // anim.blend        <- STRING_ANIM_BLEND
// Playback rate multiplier.
string::core::CVar<float>& cv_anim_rate();          // anim.rate         <- STRING_ANIM_RATE
// Auto demo: cross-fade Idle_Loop <-> Walk_Loop every 3 s while anim.clip is empty, so ./run.sh
// demonstrates blending with no input (and captures hit a deterministic frame). 0 = hold idle.
string::core::CVar<int32_t>& cv_anim_demo();        // anim.demo         <- STRING_ANIM_DEMO

}  // namespace sandbox
