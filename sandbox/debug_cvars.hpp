#pragma once

#include <string>

#include <string/core/cvar.hpp>

// Sandbox debug CVars (brief 06). Every improvised STRING_* lever that used to be read via
// std::getenv in the sandbox passes is now a first-class, self-registering CVar under a canonical
// dotted name (consistent with the engine's r.capture.* convention). The LEGACY env name is kept
// working verbatim via an alias, so existing agent recipes and muscle-memory launch lines are
// unchanged: e.g. STRING_HIZ=0 still resolves (alias "hiz" -> r.hiz.enabled) exactly as before.
//
// Accessor pattern (mirrors renderer.cpp): each CVar is a function-local static so it registers
// exactly once, aliases itself once, and is safe to touch from any TU / any order. Call sites read
// the value with .get(); the env bridge (CVarRegistry::apply_env, run once at startup) applies the
// STRING_<NAME> / STRING_<ALIAS> overrides before the passes read them.
namespace sandbox
{

// Registers every sandbox debug CVar (and its legacy alias) and applies the env bridge. Call once,
// early in startup, before the passes are constructed so the seeded values are already overridden.
void register_debug_cvars();

// --- Culling / LOD / HiZ (geometry pass) -----------------------------------------------------
string::core::CVar<bool>&    cv_hiz_enabled();      // r.hiz.enabled       <- STRING_HIZ
string::core::CVar<bool>&    cv_lod_enabled();      // r.lod.enabled       <- STRING_LOD
string::core::CVar<bool>&    cv_cull_enabled();     // r.cull.enabled      <- STRING_CULL
string::core::CVar<float>&   cv_lod_error_px();     // r.lod.error_px      <- STRING_LOD_PX
string::core::CVar<bool>&    cv_meshlet_validate(); // dbg.meshlet_validate<- STRING_MESHLET_VALIDATE
string::core::CVar<bool>&    cv_meshlet_readback(); // dbg.meshlet_readback<- STRING_MESHLET_READBACK
string::core::CVar<int32_t>& cv_meshlet_dump();     // dbg.meshlet_dump    <- STRING_MESHLET_DUMP (-1 off)

// --- Lighting / scene composition ------------------------------------------------------------
string::core::CVar<bool>&    cv_lights_enabled();   // r.lights.enabled    <- STRING_LIGHTS
string::core::CVar<bool>&    cv_crowd_enabled();    // r.crowd.enabled     <- STRING_CROWD
string::core::CVar<int32_t>& cv_chunk_budget();     // r.chunk.budget      <- STRING_CHUNK

// --- Debug view / camera ---------------------------------------------------------------------
string::core::CVar<int32_t>& cv_debug_view();       // r.debug.view        <- STRING_VIEW
string::core::CVar<std::string>& cv_camera_pose();  // r.camera.pose       <- STRING_CAM
// Debug/tooling motion lever: when nonzero, orbits/sways the demo camera continuously at this
// angular speed (rad/s) around its current pose (small radius) so headless captures exercise
// per-frame disocclusion (the two-phase phase-1/phase-2 interleaved path). 0 = off (static).
string::core::CVar<float>&   cv_orbit();            // dbg.orbit           <- STRING_ORBIT

// --- UI (brief 05) ---------------------------------------------------------------------------
// Scene selector: "demo" = full Sponza + UI; "ui" = UI-dev sandbox (background + UI passes only,
// synthetic world anchors, near-instant startup). STRING_SCENE=ui ./run.sh is the UI sandbox.
string::core::CVar<std::string>& cv_scene();        // dbg.scene           <- STRING_SCENE
// Synthetic nameplate stress count for the ui-dev scene's nameplate screen (0 = off).
string::core::CVar<int32_t>& cv_ui_nameplates();    // dbg.ui.nameplates   <- STRING_UI_NAMEPLATES
// Which mock screen the ui-dev scene shows: nameplates | inventory | actionbar | chat | all.
string::core::CVar<std::string>& cv_ui_screen();    // dbg.ui.screen       <- STRING_UI_SCREEN

// --- Transparency A/B verification -----------------------------------------------------------
string::core::CVar<bool>&    cv_transp_test();      // dbg.transp_test     <- STRING_TRANSP_TEST
string::core::CVar<bool>&    cv_transp_reverse();   // dbg.transp_reverse  <- STRING_TRANSP_REVERSE

// --- Inspector (brief 06) --------------------------------------------------------------------
// Isolate-draw render mode: when >=0 the draw-cull compute keeps only this draw index and culls
// all others (bisection tool the inspector drives). -1 = off. <- STRING_ISOLATE_DRAW
string::core::CVar<int32_t>& cv_isolate_draw();     // dbg.isolate_draw    <- STRING_ISOLATE_DRAW

// --- Debug tooling surfaces (brief 06) -------------------------------------------------------
// HUD (per-pass GPU ms + frame stats): toggled by CVar + F2. Inspector (draw table + lights):
// toggled by CVar + F3. Both default off so headless captures are clean unless asked for.
string::core::CVar<bool>&    cv_hud_enabled();      // dbg.hud             <- STRING_HUD
string::core::CVar<bool>&    cv_logs_enabled();     // dbg.logs            <- STRING_LOGS (F4)
string::core::CVar<bool>&    cv_inspector_enabled();// dbg.inspector       <- STRING_INSPECTOR
// Console open state: normally toggled by the grave key at runtime, but exposed as a CVar so a
// headless capture can open it (STRING_CONSOLE=1) for screenshot verification.
string::core::CVar<bool>&    cv_console_open();     // dbg.console         <- STRING_CONSOLE
// Debug-draw self-test pattern (axes + AABB + sphere at the origin) so the immediate-mode debug
// pipeline can be verified from a headless capture. <- STRING_DRAW_TEST
string::core::CVar<bool>&    cv_draw_test();        // dbg.draw_test       <- STRING_DRAW_TEST

}  // namespace sandbox
