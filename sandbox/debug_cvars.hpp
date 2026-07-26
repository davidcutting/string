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

// --- PBR / IBL (brief 07) ---------------------------------------------------------------------
// White-furnace test: uniform white environment, sun + local lights off, albedo forced white,
// AO off. An energy-conserving BRDF makes the scene disappear into the background — the
// acceptance gate for the brief-07 BRDF work.
string::core::CVar<bool>&    cv_furnace();          // r.furnace           <- STRING_FURNACE
// Numeric IBL verification: at a settled frame, read the DFG LUT + SH coefficients back and check
// them against CPU references (uniform-env SH DC, DFG corner/interior values). Logs PASS/FAIL.
string::core::CVar<bool>&    cv_ibl_verify();       // dbg.ibl_verify      <- STRING_IBL_VERIFY
// Force the IBL update chain (capture/prefilter/SH) to re-run EVERY frame regardless of the
// sun-delta trigger — for measuring the worst-case per-frame cost (Tracy "ibl-update" zone).
string::core::CVar<bool>&    cv_ibl_every_frame();  // dbg.ibl_every_frame <- STRING_IBL_EVERY_FRAME
// Headless time-of-day: >= 0 pins time_of_day_ to this value (0..1); < 0 = scene default.
string::core::CVar<float>&   cv_time_of_day();      // r.tod               <- STRING_TOD
// Sun arc tilt toward south (radians): great-circle day path. Smaller = higher noon sun.
string::core::CVar<float>&   cv_sun_lean();         // r.sun.lean          <- STRING_SUN_LEAN
// Headless sun animation: start with time-of-day advancing (the T key toggle), for TOD motion
// sequences without input.
string::core::CVar<bool>&    cv_sun_animate();      // dbg.sun_animate     <- STRING_SUN_ANIM

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

// --- Post-processing (brief 09) ----------------------------------------------------------------
// Bloom: threshold-free Karis downsample/upsample chain, applied into the HDR target pre-composite.
string::core::CVar<bool>&    cv_bloom_enabled();    // r.bloom.enabled     <- STRING_BLOOM
string::core::CVar<float>&   cv_bloom_intensity();  // r.bloom.intensity   <- STRING_BLOOM_INTENSITY
string::core::CVar<float>&   cv_bloom_clamp();      // r.bloom.clamp       <- STRING_BLOOM_CLAMP (knits)
string::core::CVar<float>&   cv_bloom_radius();     // r.bloom.radius      <- STRING_BLOOM_RADIUS
string::core::CVar<int32_t>& cv_bloom_mips();       // r.bloom.mips        <- STRING_BLOOM_MIPS
// Auto-exposure metering (histogram): percentile trim + EV clamps + adaptation rates. The
// enable lever is engine-side (r.exposure.auto, composite_pass.cpp).
string::core::CVar<float>&   cv_exposure_min_ev();  // r.exposure.min_ev   <- STRING_EXPOSURE_MIN_EV
string::core::CVar<float>&   cv_exposure_max_ev();  // r.exposure.max_ev   <- STRING_EXPOSURE_MAX_EV
string::core::CVar<float>&   cv_exposure_speed_up();   // r.exposure.speed_up   <- STRING_EXPOSURE_SPEED_UP
string::core::CVar<float>&   cv_exposure_speed_down(); // r.exposure.speed_down <- STRING_EXPOSURE_SPEED_DOWN
string::core::CVar<float>&   cv_exposure_comp();    // r.exposure.comp     <- STRING_EXPOSURE_COMP (stops)
string::core::CVar<float>&   cv_exposure_cut_low(); // r.exposure.cut_low  <- STRING_EXPOSURE_CUT_LOW
string::core::CVar<float>&   cv_exposure_cut_high();// r.exposure.cut_high <- STRING_EXPOSURE_CUT_HIGH
// One-shot histogram sanity log (total == pixel count).
string::core::CVar<bool>&    cv_exposure_verify();  // dbg.exposure_verify <- STRING_EXPOSURE_VERIFY
// GTAO (half-res, bent normals; consumed by lighting.slang's ambient terms).
string::core::CVar<bool>&    cv_gtao_enabled();     // r.gtao.enabled      <- STRING_GTAO
string::core::CVar<float>&   cv_gtao_strength();    // r.gtao.strength     <- STRING_GTAO_STRENGTH
string::core::CVar<float>&   cv_gtao_radius();      // r.gtao.radius       <- STRING_GTAO_RADIUS (world)
// Bent-normal specular occlusion (Lagarde cone vs reflection lobe). Off = fall back to brief-07's
// AO-derived Lagarde spec-occ (spec_ao only). A/B lever for the mirror-reflection defect.
string::core::CVar<bool>&    cv_gtao_spec_occ();    // r.gtao.spec_occ     <- STRING_GTAO_SPEC_OCC
string::core::CVar<float>&   cv_shadow_bias();      // r.shadow.bias       <- STRING_SHADOW_BIAS
string::core::CVar<float>&   cv_shadow_normal_offset();// r.shadow.normal_offset <- STRING_SHADOW_NORMAL_OFFSET
string::core::CVar<int32_t>& cv_light_debug();       // r.debug.lighting    <- STRING_DEBUG_LIGHTING

// --- Probe GI (brief 09b): relightable irradiance volume -------------------------------------
string::core::CVar<bool>&    cv_gi_enabled();       // r.gi                <- STRING_GI
string::core::CVar<bool>&    cv_pass_geometry();    // r.pass.geometry <- STRING_R_PASS_GEOMETRY (brief 11 P2)
string::core::CVar<float>&   cv_gi_spacing();       // r.gi.spacing        <- STRING_GI_SPACING (world m)
string::core::CVar<float>&   cv_gi_hysteresis();    // r.gi.hysteresis     <- STRING_GI_HYSTERESIS (0..1)
string::core::CVar<float>&   cv_gi_occluded_floor();// r.gi.occluded_floor <- STRING_GI_OCCLUDED_FLOOR (0..1)
// Debug probe spheres: 0 off, 1 flat grey (placement), 2 irradiance, 3 visibility.
string::core::CVar<int32_t>& cv_gi_probe_debug();   // r.gi.probe_debug    <- STRING_GI_PROBE_DEBUG
// GI debug view: 0 off, 1 = indirect diffuse (probe irradiance x albedo) only.
string::core::CVar<int32_t>& cv_gi_debug();         // r.gi.debug          <- STRING_GI_DEBUG

}  // namespace sandbox
