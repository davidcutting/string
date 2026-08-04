#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <string/platform/input.hpp>
#include <string/platform/input_map.hpp>
#include <string/ui/interaction.hpp>
#include <string/ui/layout.hpp>
#include <string/ui/motion.hpp>
#include <string/ui/panel.hpp>
#include <string/ui/ui.hpp>

#include <string/vulkan/lens.hpp>

#include <string/debug/console.hpp>

namespace string::debug
{

// The debug shell: console, log view, frame-graph view, profiler HUD, menu bar, and the inspector
// SLOT. One persistent state object lives in the UI author closure and drives them all each frame.
// The console is modal (grave/backtick toggles; while open it sets Input::text_capture so gameplay
// input is suppressed). The rest are CVar+function-key toggled, all defaulting off.
//
// Every surface here is RENDERER-AGNOSTIC. The frame-graph view and the HUD read the engine's
// process-global GraphIntrospect/GpuProfiler handles, which is what lets this library depend on
// nothing but the engine core and the UI kit. Anything that needs a specific renderer's numbers
// arrives through the two slots below, filled by whoever is rendering (see string-render-forward's
// RenderDebug, wired up by the app).
class DebugPanels
{
public:
    DebugPanels();

    // Rows appended inside the profiler HUD, under the generic per-pass GPU timings.
    using PanelAuthor = std::function<void(::string::ui::Ui&)>;
    void set_hud_extra(PanelAuthor a) { hud_extra_ = std::move(a); }
    // Contents of the scene/draw inspector panel. Unset => the panel says so rather than vanishing,
    // because a missing inspector is a wiring mistake, not a state worth hiding.
    void set_inspector(PanelAuthor a) { inspector_ = std::move(a); }

    // Called once per frame from the UI author. Reads input (toggles, console typing), then authors
    // whatever surfaces are open into `b`. `input` is non-const so the console can take text
    // capture; `ui` is the resolved interaction for this frame.
    // Authors into the APP'S Ui, not one of its own.
    //
    // It used to build a second `Ui` over the same builder, which cost more than duplication: two
    // `PanelStore`s meant the app's panels and these ranked z INDEPENDENTLY, so raising one could
    // not reliably put it above the other — and every per-frame hand-off a Ui owns (`observe`,
    // `clipboard_write`) had to be remembered twice, which was silently forgotten twice.
    //
    // One Ui per frame is also what brief 12b's M1 surface registry needs to hang off something.
    void update_and_author(::string::ui::Ui& u, ::String::Input& input,
                           ::String::InputMap& input_map);

private:
    // The ONE writer for the console's open state — it is mirrored in a CVar, so a direct member
    // assignment gets reconciled away a frame later. See the definition.
    void set_console_open(bool open);
    void handle_toggles(const ::String::Input& in);
    void handle_console_input(const ::String::Input& in);
    void author_menu_bar(::string::ui::Ui& u);
    void author_console(::string::ui::Ui& u);
    void author_logs(::string::ui::Ui& u);
    void author_hud(::string::ui::Ui& u);
    void author_graph(::string::ui::Ui& u);
    void author_dag(::string::ui::Ui& u);
    void author_image(::string::ui::Ui& u);
    // Binding inspector + rebinder. Takes the raw Input as well as the map because "bind the next key
    // I press" is the one question that is genuinely about a KEY rather than an action.
    void author_bindings(::string::ui::Ui& u, const ::String::Input& input,
                         ::String::InputMap& map);
    // System actions: the ones nothing may swallow. Runs before every surface.
    void handle_system_actions(::String::InputMap& map);
    void author_lenses(::string::ui::Ui& u, ::string::dimension screen);
    void author_inspector(::string::ui::Ui& u);

    DebugConsole console_;
    bool console_open_ = false;
    std::string input_;               // current console edit line


    // DAG selection. Exactly one is ever set — selecting a pass clears the resource and vice versa,
    // so the two highlight schemes can never overlap and mean different things at once.
    // Rebinder state: the action waiting for a key, and the input CONTEXT it pushes while waiting.
    // Zero id = not capturing.
    ::String::ActionId rebinding_{};
    bool system_bound_ = false;
    int sel_pass_ = -1;
    int sel_res_ = -1;

    // Image-browser controls (brief 14 M4). Channel defaults to R, not RGBA: the glyph atlas is a
    // single R8 channel, and the sampler reports the absent green/blue as 0 — so an RGBA view of it
    // renders RED, which reads as "the data is in the red channel" rather than "this texture has
    // one channel". A one-bit mask is drawn as grey (see image_shader), which is the honest view.
    int img_channel_ = 1;
    float img_mip_ = 0.0f;
    float img_min_ = 0.0f;
    float img_max_ = 1.0f;
    bool img_false_colour_ = false;

    PanelAuthor hud_extra_;
    PanelAuthor inspector_;


    // True for the single frame the grave key toggled the console, so the keystroke that opened it
    // is not also typed into it. See handle_toggles.
    bool swallow_keystroke_ = false;
    bool prev_grave_ = false;         // grave-key edge detection (raw input, not InputMap)
    bool prev_f1_ = false;
    bool prev_f2_ = false;
    bool prev_f3_ = false;
    bool prev_f4_ = false;
    bool prev_f5_ = false;
    bool prev_f6_ = false;
    bool prev_f7_ = false;
    bool prev_f8_ = false;
    bool prev_tab_ = false;
    bool prev_enter_ = false;
    bool prev_up_ = false;
    bool prev_down_ = false;
};

}  // namespace string::debug
