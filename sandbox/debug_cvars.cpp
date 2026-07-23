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
    static CVar<bool> v{"r.lights.enabled", true, "local-light stress set (L toggles at runtime)"};
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
                               "scene selector: \"demo\" (Sponza + UI) or \"ui\" (UI-dev sandbox)"};
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
    cv_transp_test();
    cv_transp_reverse();
    cv_isolate_draw();
    cv_hud_enabled();
    cv_logs_enabled();
    cv_inspector_enabled();
    cv_console_open();
    cv_draw_test();
    string::core::CVarRegistry::instance().apply_env();
}

}  // namespace sandbox
