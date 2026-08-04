#include <string/debug/debug_cvars.hpp>

namespace string::debug
{

using ::string::core::CVar;
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
CVar<bool>& cv_graph_enabled()
{
    static CVar<bool> v{"dbg.graph", false, "render-graph resource lifetime timeline (F5 toggles)"};
    static const bool a = [] { v.add_alias("graph"); return true; }();
    (void)a;
    return v;
}
CVar<bool>& cv_menu_enabled()
{
    static CVar<bool> v{"dbg.menu", false, "debug menu bar (F1 toggles)"};
    static const bool a = [] { v.add_alias("menu"); return true; }();
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
CVar<bool>& cv_dag_enabled()
{
    static CVar<bool> v{"dbg.dag", false, "frame-graph pass DAG panel (F6 toggles)"};
    static const bool a = [] { v.add_alias("dag"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_image_enabled()
{
    static CVar<bool> v{"dbg.image", false, "image / target browser panel (F7 toggles)"};
    static const bool a = [] { v.add_alias("image"); return true; }();
    (void)a;
    return v;
}

CVar<bool>& cv_bindings_enabled()
{
    static CVar<bool> v{"dbg.bindings", false, "input binding inspector / rebinder (F8 toggles)"};
    static const bool a = [] { v.add_alias("bindings"); return true; }();
    (void)a;
    return v;
}

CVar<int32_t>& cv_lens_test()
{
    // Same purpose as dbg.draw_test: seed a deterministic lens so the substitution path can be
    // verified from a HEADLESS capture. A feature only reachable by dragging a rect with a mouse
    // cannot be gated, and "it looked right" is not a gate.
    //
    // The VALUE is the magnification, which makes the sharpest test expressible: at 1x the sampling
    // must reduce to identity, so the lens interior has to come out BYTE-IDENTICAL to a no-lens
    // capture. That checks the formula itself, not merely that pixels changed.
    static CVar<int32_t> v{"dbg.lens_test", 0,
                        "seed one lens at a fixed rect with THIS magnification (0 = off); headless "
                        "verification of the composite's lens substitution"};
    static const bool a = [] { v.add_alias("lens_test"); return true; }();
    (void)a;
    return v;
}

void register_debug_cvars()
{
    cv_lens_test();
    cv_image_enabled();
    cv_bindings_enabled();
    cv_dag_enabled();
    cv_hud_enabled();
    cv_logs_enabled();
    cv_graph_enabled();
    cv_menu_enabled();
    cv_inspector_enabled();
    cv_console_open();
    cv_draw_test();
}

}  // namespace string::debug
