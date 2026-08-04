#pragma once

#include <string/core/cvar.hpp>

namespace string::debug
{

// The debug SHELL's own CVars: which surface is open. Each also has a function key; all default
// OFF so a headless capture is clean unless it asks for one.
//
// These live with the surfaces they toggle, not with the app, for the same reason the renderer's
// tuning cvars live with the renderer: whoever owns the feature owns its levers.
//
// Registration is pure — the app calls CVarRegistry::apply_env() once, after every library has
// registered (see the note in main.cpp; the order is load-bearing).
void register_debug_cvars();

::string::core::CVar<bool>& cv_hud_enabled();       // dbg.hud       <- STRING_HUD (F2)
::string::core::CVar<bool>& cv_logs_enabled();      // dbg.logs      <- STRING_LOGS (F4)
::string::core::CVar<bool>& cv_graph_enabled();     // dbg.graph     <- STRING_GRAPH (F5)
string::core::CVar<bool>& cv_dag_enabled();       // dbg.dag       <- STRING_DAG (F6)
string::core::CVar<bool>& cv_image_enabled();     // dbg.image     <- STRING_IMAGE (F7)
string::core::CVar<bool>& cv_bindings_enabled();  // dbg.bindings  <- STRING_BINDINGS (F8)
::string::core::CVar<bool>& cv_menu_enabled();      // dbg.menu      <- STRING_MENU (F1)
::string::core::CVar<bool>& cv_inspector_enabled(); // dbg.inspector <- STRING_INSPECTOR (F3)
::string::core::CVar<bool>& cv_console_open();      // dbg.console   <- STRING_CONSOLE
::string::core::CVar<bool>& cv_draw_test();         // dbg.draw_test <- STRING_DRAW_TEST
string::core::CVar<int32_t>& cv_lens_test();      // dbg.lens_test <- STRING_LENS_TEST (0=off, else magnification)

}  // namespace string::debug
