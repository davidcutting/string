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
void register_debug_cvars()
{
    cv_scene();
    cv_ui_nameplates();
}

}  // namespace sandbox
