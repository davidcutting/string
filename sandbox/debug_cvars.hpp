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

}  // namespace sandbox
