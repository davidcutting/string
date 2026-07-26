#include "debug_cvars.hpp"

namespace sandbox
{

using string::core::CVar;
using string::core::CVarFlags;

// Each accessor is a function-local static: registered + aliased exactly once, order-independent.
// The alias string is the LEGACY env token *without* the STRING_ prefix (the env bridge upper-cases
// it and prepends STRING_), so add_alias("hiz") honours STRING_HIZ verbatim.

CVar<bool>& cv_hiz_enabled()
{
    static CVar<bool> v{"r.hiz.enabled", true, "meshlet HiZ occlusion cull (O toggles at runtime)"};
    static const bool a = [] { v.add_alias("hiz"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_lod_enabled()
{
    static CVar<bool> v{"r.lod.enabled", true, "discrete LOD select (G toggles; off = force LOD0)"};
    static const bool a = [] { v.add_alias("lod"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_cull_enabled()
{
    static CVar<bool> v{"r.cull.enabled", true, "GPU draw/meshlet frustum cull (C toggles at runtime)"};
    static const bool a = [] { v.add_alias("cull"); return true; }();
    (void)a;
    return v;
}

CVar<float>& cv_lod_error_px()
{
    static CVar<float> v{"r.lod.error_px", 0.02f, "screen-space LOD error budget in pixels"};
    static const bool a = [] { v.add_alias("lod_px"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_meshlet_validate()
{
    static CVar<bool> v{"dbg.meshlet_validate", false, "run meshlet-builder validation at load"};
    static const bool a = [] { v.add_alias("meshlet_validate"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_meshlet_readback()
{
    static CVar<bool> v{"dbg.meshlet_readback", false, "read meshlet buffers back and memcmp after upload"};
    static const bool a = [] { v.add_alias("meshlet_readback"); return true; }();
    (void)a;
    return v;
}

CVar<int32_t>& cv_meshlet_dump()
{
    // Legacy STRING_MESHLET_DUMP was presence-gated with a start draw index; -1 = off, >=0 = start.
    static CVar<int32_t> v{"dbg.meshlet_dump", -1, "dump 6 draws' DrawInfo from this start index (-1 off)"};
    static const bool a = [] { v.add_alias("meshlet_dump"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_lights_enabled()
{
    static CVar<bool> v{"r.lights.enabled", false, "local-light stress set (L toggles at runtime; STRING_LIGHTS=1 to force on)"};
    static const bool a = [] { v.add_alias("lights"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_crowd_enabled()
{
    static CVar<bool> v{"r.crowd.enabled", false, "crowd stress scene (K toggles at runtime)"};
    static const bool a = [] { v.add_alias("crowd"); return true; }();
    (void)a;
    return v;
}

CVar<int32_t>& cv_chunk_budget()
{
    // Brief 04c M4: FINALIZED OFF. Chunking splits draws into more per-draw commands; under
    // resolution (b)'s per-draw command boundaries (required for determinism) that fan-out is NOT
    // free (interior chunked costs ~+1.1 ms main-draw + ~+0.7 ms shadow-cascades vs unchunked, with
    // no quality/determinism benefit — the single-dispatch premise that would have made it free was
    // rejected by the determinism gate). 0 selects the unchunked cooked variant. Kept as a CVar for
    // A/B measurement (STRING_CHUNK=1024 restores the chunked variant).
    static CVar<int32_t> v{"r.chunk.budget", 0,
                           "spatial chunk meshlet budget; 0 = unchunked cooked variant (default)"};
    static const bool a = [] { v.add_alias("chunk"); return true; }();
    (void)a;
    return v;
}

CVar<int32_t>& cv_debug_view()
{
    static CVar<int32_t> v{"r.debug.view", 0,
                           "meshlet debug view: 0 none, 1 meshlet, 2 LOD, 3 occlusion-reject (V cycles)"};
    static const bool a = [] { v.add_alias("view"); return true; }();
    (void)a;
    return v;
}

CVar<std::string>& cv_camera_pose()
{
    static CVar<std::string> v{"r.camera.pose", "",
                               "startup camera pose \"px,py,pz,yaw,pitch\" (radians); empty = scene default"};
    static const bool a = [] { v.add_alias("cam"); return true; }();
    (void)a;
    return v;
}

CVar<float>& cv_orbit()
{
    static CVar<float> v{"dbg.orbit", 0.0f,
                         "orbit/sway the demo camera continuously at this angular speed (rad/s) "
                         "around its current pose; 0 = off. Headless motion lever for two-phase disocclusion."};
    static const bool a = [] { v.add_alias("orbit"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_furnace()
{
    static CVar<bool> v{"r.furnace", false,
                        "white-furnace test: uniform white env, no sun/lights, white albedo (brief 07)"};
    static const bool a = [] { v.add_alias("furnace"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_ibl_verify()
{
    static CVar<bool> v{"dbg.ibl_verify", false,
                        "read back DFG LUT + SH coefficients and check against CPU references"};
    static const bool a = [] { v.add_alias("ibl_verify"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_ibl_every_frame()
{
    static CVar<bool> v{"dbg.ibl_every_frame", false,
                        "re-run the IBL update chain every frame (worst-case cost measurement)"};
    static const bool a = [] { v.add_alias("ibl_every_frame"); return true; }();
    (void)a;
    return v;
}

CVar<float>& cv_time_of_day()
{
    static CVar<float> v{"r.tod", -1.0f,
                         "pin time-of-day to this value in [0,1] (headless TOD captures); <0 = default"};
    static const bool a = [] { v.add_alias("tod"); return true; }();
    (void)a;
    return v;
}

CVar<float>& cv_sun_lean()
{
    static CVar<float> v{"r.sun.lean", 0.6f,
                         "sun arc tilt toward south (radians); smaller = higher noon sun, ~0 = overhead"};
    static const bool a = [] { v.add_alias("sun_lean"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_sun_animate()
{
    static CVar<bool> v{"dbg.sun_animate", false,
                        "start with time-of-day animating (the T toggle), for headless TOD sequences"};
    static const bool a = [] { v.add_alias("sun_anim"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_transp_test()
{
    static CVar<bool> v{"dbg.transp_test", false, "inject synthetic alpha-blended quads for sort verify"};
    static const bool a = [] { v.add_alias("transp_test"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_transp_reverse()
{
    static CVar<bool> v{"dbg.transp_reverse", false, "reverse transparency sort order (A/B correctness check)"};
    static const bool a = [] { v.add_alias("transp_reverse"); return true; }();
    (void)a;
    return v;
}

CVar<int32_t>& cv_isolate_draw()
{
    static CVar<int32_t> v{"dbg.isolate_draw", -1,
                           "isolate-draw render mode: >=0 keeps only that draw index, -1 = off"};
    static const bool a = [] { v.add_alias("isolate_draw"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_hud_enabled()
{
    static CVar<bool> v{"dbg.hud", false, "profiler HUD: per-pass GPU ms + frame stats (F2 toggles)"};
    static const bool a = [] { v.add_alias("hud"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_logs_enabled()
{
    static CVar<bool> v{"dbg.logs", false, "log tail panel, newest first (F4 toggles)"};
    static const bool a = [] { v.add_alias("logs"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_inspector_enabled()
{
    static CVar<bool> v{"dbg.inspector", false, "scene/draw inspector panel (F3 toggles)"};
    static const bool a = [] { v.add_alias("inspector"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_console_open()
{
    static CVar<bool> v{"dbg.console", false, "console open state (grave toggles; STRING_CONSOLE=1 opens headless)"};
    static const bool a = [] { v.add_alias("console"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_draw_test()
{
    static CVar<bool> v{"dbg.draw_test", false, "emit a debug-draw self-test pattern at the origin"};
    static const bool a = [] { v.add_alias("draw_test"); return true; }();
    (void)a;
    return v;
}

CVar<std::string>& cv_scene()
{
    static CVar<std::string> v{"dbg.scene", "demo",
                               "scene selector: \"demo\" (Sponza + UI), \"ui\" (UI-dev sandbox) or "
                               "\"lookdev\" (roughness x metallic sphere grid, brief 07)"};
    static const bool a = [] { v.add_alias("scene"); return true; }();
    (void)a;
    return v;
}

CVar<int32_t>& cv_ui_nameplates()
{
    static CVar<int32_t> v{"dbg.ui.nameplates", 0,
                           "synthetic nameplate stress count in the ui-dev scene (0 = off)"};
    static const bool a = [] { v.add_alias("ui_nameplates"); return true; }();
    (void)a;
    return v;
}

CVar<std::string>& cv_ui_screen()
{
    static CVar<std::string> v{"dbg.ui.screen", "all",
                               "ui-dev mock screen: nameplates|inventory|actionbar|chat|all"};
    static const bool a = [] { v.add_alias("ui_screen"); return true; }();
    (void)a;
    return v;
}

// --- Post-processing (brief 09) ------------------------------------------------------------------
CVar<bool>& cv_bloom_enabled()
{
    static CVar<bool> v{"r.bloom.enabled", true, "Karis bloom chain (threshold-free, clamped)"};
    static const bool a = [] { v.add_alias("bloom"); return true; }(); (void)a;
    return v;
}
CVar<float>& cv_bloom_intensity()
{
    static CVar<float> v{"r.bloom.intensity", 0.04f, "bloom contribution added to the scene"};
    static const bool a = [] { v.add_alias("bloom_intensity"); return true; }(); (void)a;
    return v;
}
CVar<float>& cv_bloom_clamp()
{
    static CVar<float> v{"r.bloom.clamp", 64.0f,
                         "firefly/haze clamp on the first bloom downsample (scene knits)"};
    static const bool a = [] { v.add_alias("bloom_clamp"); return true; }(); (void)a;
    return v;
}
CVar<float>& cv_bloom_radius()
{
    static CVar<float> v{"r.bloom.radius", 0.85f,
                         "per-level upsample-accumulate scale (lower = tighter glow)"};
    static const bool a = [] { v.add_alias("bloom_radius"); return true; }(); (void)a;
    return v;
}
CVar<int32_t>& cv_bloom_mips()
{
    static CVar<int32_t> v{"r.bloom.mips", 6, "bloom pyramid depth (1..8; smallest mip >= 4px)"};
    static const bool a = [] { v.add_alias("bloom_mips"); return true; }(); (void)a;
    return v;
}
CVar<float>& cv_exposure_min_ev()
{
    static CVar<float> v{"r.exposure.min_ev", 6.0f, "auto-exposure EV100 floor (night)"};
    static const bool a = [] { v.add_alias("exposure_min_ev"); return true; }(); (void)a;
    return v;
}
CVar<float>& cv_exposure_max_ev()
{
    static CVar<float> v{"r.exposure.max_ev", 17.0f, "auto-exposure EV100 ceiling (noon sun)"};
    static const bool a = [] { v.add_alias("exposure_max_ev"); return true; }(); (void)a;
    return v;
}
CVar<float>& cv_exposure_speed_up()
{
    static CVar<float> v{"r.exposure.speed_up", 3.0f,
                         "adaptation rate when EV rises (scene brightened), 1/s"};
    static const bool a = [] { v.add_alias("exposure_speed_up"); return true; }(); (void)a;
    return v;
}
CVar<float>& cv_exposure_speed_down()
{
    static CVar<float> v{"r.exposure.speed_down", 1.5f,
                         "adaptation rate when EV falls (dark adaptation), 1/s"};
    static const bool a = [] { v.add_alias("exposure_speed_down"); return true; }(); (void)a;
    return v;
}
CVar<float>& cv_exposure_comp()
{
    static CVar<float> v{"r.exposure.comp", 0.0f, "exposure compensation (stops; + brightens)"};
    static const bool a = [] { v.add_alias("exposure_comp"); return true; }(); (void)a;
    return v;
}
CVar<float>& cv_exposure_cut_low()
{
    static CVar<float> v{"r.exposure.cut_low", 0.50f,
                         "histogram percentile trimmed from the dark end before averaging"};
    static const bool a = [] { v.add_alias("exposure_cut_low"); return true; }(); (void)a;
    return v;
}
CVar<float>& cv_exposure_cut_high()
{
    static CVar<float> v{"r.exposure.cut_high", 0.05f,
                         "histogram percentile trimmed from the bright end (sun/sky)"};
    static const bool a = [] { v.add_alias("exposure_cut_high"); return true; }(); (void)a;
    return v;
}
CVar<bool>& cv_exposure_verify()
{
    static CVar<bool> v{"dbg.exposure_verify", false,
                        "one-shot log: histogram total vs pixel count + metering value"};
    static const bool a = [] { v.add_alias("exposure_verify"); return true; }(); (void)a;
    return v;
}
CVar<bool>& cv_gtao_enabled()
{
    static CVar<bool> v{"r.gtao.enabled", true,
                        "half-res GTAO with bent normals (needs the HiZ two-phase depth)"};
    static const bool a = [] { v.add_alias("gtao"); return true; }(); (void)a;
    return v;
}
CVar<float>& cv_gtao_strength()
{
    static CVar<float> v{"r.gtao.strength", 1.0f, "GTAO strength (0 = off, 1 = full visibility)"};
    static const bool a = [] { v.add_alias("gtao_strength"); return true; }(); (void)a;
    return v;
}
CVar<float>& cv_gtao_radius()
{
    static CVar<float> v{"r.gtao.radius", 0.8f, "GTAO world-space sampling radius (meters)"};
    static const bool a = [] { v.add_alias("gtao_radius"); return true; }(); (void)a;
    return v;
}
CVar<bool>& cv_gtao_spec_occ()
{
    static CVar<bool> v{"r.gtao.spec_occ", true,
                        "bent-normal specular occlusion (off = brief-07 AO-derived Lagarde fallback)"};
    static const bool a = [] { v.add_alias("gtao_spec_occ"); return true; }(); (void)a;
    return v;
}
CVar<int32_t>& cv_light_debug()
{
    // Lighting isolate view: 0 normal, 1 sun-direct only, 2 ambient only, 3 local (froxel) lights,
    // 4 sun shadow coverage (white = lit, black = in shadow / back-facing), 5 selected cascade,
    // 6 occluder probe (green = caster above pixel, red = pixel topmost in map / caster drop).
    static CVar<int32_t> v{"r.debug.lighting", 0,
                           "lighting isolate: 0 off, 1 sun-direct, 2 ambient, 3 local, 4 shadow coverage, 5 cascade, 6 occluder-probe"};
    static const bool a = [] { v.add_alias("light_debug"); return true; }(); (void)a;
    return v;
}
CVar<float>& cv_shadow_bias()
{
    // Constant depth bias (reverse-Z units), slope-scaled up to 3x at grazing in-shader. Higher =
    // less acne, more peter-panning (sun leaks onto faces geometry should self-shadow).
    static CVar<float> v{"r.shadow.bias", 0.0006f, "CSM constant depth bias (reverse-Z units)"};
    static const bool a = [] { v.add_alias("shadow_bias"); return true; }(); (void)a;
    return v;
}
CVar<float>& cv_shadow_normal_offset()
{
    // Shadow sample pushed this many cascade world-texels along the surface normal. Higher = less
    // acne, more leak. Typical 1-2; 3 leaks on grazing faces (the "lit interior pillar face").
    static CVar<float> v{"r.shadow.normal_offset", 0.8f, "CSM normal-offset bias (cascade world-texels)"};
    static const bool a = [] { v.add_alias("shadow_normal_offset"); return true; }(); (void)a;
    return v;
}

CVar<bool>& cv_gi_enabled()
{
    static CVar<bool> v{"r.gi", true, "probe GI: relightable irradiance volume (brief 09b)"};
    static const bool a = [] { v.add_alias("gi"); return true; }(); (void)a;
    return v;
}

CVar<bool>& cv_pass_geometry()
{
    static CVar<bool> v{"r.pass.geometry", true, "toggle the geometry pass (brief 11 Phase 2 per-pass enable/disable)"};
    return v;
}
CVar<float>& cv_gi_spacing()
{
    static CVar<float> v{"r.gi.spacing", 2.0f, "probe grid spacing in world meters (0 = auto)"};
    static const bool a = [] { v.add_alias("gi_spacing"); return true; }(); (void)a;
    return v;
}
CVar<float>& cv_gi_hysteresis()
{
    // 0.9 with 32 converge passes leaves h^32 ~= 3% residual when relight goes idle (0.95 left 19%).
    static CVar<float> v{"r.gi.hysteresis", 0.9f,
                         "probe relight temporal blend (fraction of previous atlas kept per update)"};
    static const bool a = [] { v.add_alias("gi_hysteresis"); return true; }(); (void)a;
    return v;
}
CVar<float>& cv_gi_occluded_floor()
{
    // Fraction of the sky-SH ambient a fully collapsed probe cage keeps (thin walls/vaults with no
    // interior-side probe). 0 = black crevices, ~1 = bright leak; ~0.3 reads as gentle occluded fill.
    static CVar<float> v{"r.gi.occluded_floor", 0.3f,
                         "fraction of sky ambient kept where the probe cage collapses (thin geometry)"};
    static const bool a = [] { v.add_alias("gi_occluded_floor"); return true; }(); (void)a;
    return v;
}
CVar<int32_t>& cv_gi_probe_debug()
{
    static CVar<int32_t> v{"r.gi.probe_debug", 0,
                           "probe debug spheres: 0 off, 1 flat grey, 2 irradiance, 3 visibility"};
    static const bool a = [] { v.add_alias("gi_probe_debug"); return true; }(); (void)a;
    return v;
}
CVar<int32_t>& cv_gi_debug()
{
    // Isolates the probe GI term on-screen so a reference-free scene becomes judgeable:
    //   0 off (normal shading), 1 = indirect diffuse irradiance x albedo ONLY (pure probe result).
    static CVar<int32_t> v{"r.gi.debug", 0,
                           "GI debug view: 0 off, 1 = indirect diffuse (probe irradiance x albedo) only"};
    static const bool a = [] { v.add_alias("gi_debug"); return true; }(); (void)a;
    return v;
}

void register_debug_cvars()
{
    // Touch every accessor so all CVars + aliases are registered, then apply the env bridge once.
    cv_hiz_enabled();
    cv_lod_enabled();
    cv_cull_enabled();
    cv_lod_error_px();
    cv_meshlet_validate();
    cv_meshlet_readback();
    cv_meshlet_dump();
    cv_lights_enabled();
    cv_crowd_enabled();
    cv_chunk_budget();
    cv_debug_view();
    cv_camera_pose();
    cv_orbit();
    cv_scene();
    cv_ui_nameplates();
    cv_ui_screen();
    cv_furnace();
    cv_ibl_verify();
    cv_ibl_every_frame();
    cv_time_of_day();
    cv_sun_lean();
    cv_sun_animate();
    cv_transp_test();
    cv_transp_reverse();
    cv_isolate_draw();
    cv_hud_enabled();
    cv_logs_enabled();
    cv_inspector_enabled();
    cv_console_open();
    cv_draw_test();
    cv_bloom_enabled();
    cv_bloom_intensity();
    cv_bloom_clamp();
    cv_bloom_radius();
    cv_bloom_mips();
    cv_exposure_min_ev();
    cv_exposure_max_ev();
    cv_exposure_speed_up();
    cv_exposure_speed_down();
    cv_exposure_comp();
    cv_exposure_cut_low();
    cv_exposure_cut_high();
    cv_exposure_verify();
    cv_gtao_enabled();
    cv_gtao_strength();
    cv_gtao_radius();
    cv_gtao_spec_occ();
    cv_shadow_bias();
    cv_shadow_normal_offset();
    cv_light_debug();
    cv_gi_enabled();
    cv_gi_spacing();
    cv_gi_hysteresis();
    cv_gi_occluded_floor();
    cv_gi_probe_debug();
    cv_gi_debug();
    cv_pass_geometry();
    string::core::CVarRegistry::instance().apply_env();
}

}  // namespace sandbox
