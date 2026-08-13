#include "debug_cvars.hpp"

namespace sandbox
{

using string::core::CVar;
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
CVar<std::string>& cv_anim_clip()
{
    static CVar<std::string> v{"anim.clip", "",
                               "clip every skinned character cross-fades to; empty = the anim.demo "
                               "auto cross-fade; an unknown name logs the available clips"};
    static const bool a = [] { v.add_alias("anim_clip"); return true; }();
    (void)a;
    return v;
}
CVar<float>& cv_anim_blend()
{
    static CVar<float> v{"anim.blend", 0.25f, "cross-fade duration, seconds (0 = snap)"};
    static const bool a = [] { v.add_alias("anim_blend"); return true; }();
    (void)a;
    return v;
}
CVar<float>& cv_anim_rate()
{
    static CVar<float> v{"anim.rate", 1.0f, "animation playback rate multiplier"};
    static const bool a = [] { v.add_alias("anim_rate"); return true; }();
    (void)a;
    return v;
}
CVar<int32_t>& cv_anim_demo()
{
    static CVar<int32_t> v{"anim.demo", 1,
                           "auto cross-fade Idle_Loop <-> Walk_Loop every 3 s while anim.clip is "
                           "empty (0 = hold the first clip)"};
    static const bool a = [] { v.add_alias("anim_demo"); return true; }();
    (void)a;
    return v;
}
void register_debug_cvars()
{
    cv_scene();
    cv_ui_nameplates();
    cv_anim_clip();
    cv_anim_blend();
    cv_anim_rate();
    cv_anim_demo();
}

}  // namespace sandbox
