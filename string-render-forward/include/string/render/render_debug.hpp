#pragma once

#include <string/ui/ui.hpp>

#include <string/render/meshlet_data.hpp>

namespace string::render
{

// ============================================================================
// This renderer's debug surfaces
// ============================================================================
//
// The debug SHELL (panels, docking, console, log view, frame-graph view, profiler HUD) is generic
// and lives in string-debug, which knows nothing about any renderer. What cannot be generic is
// anything that reads THIS renderer's numbers: meshlet cull counters, the per-draw table, the light
// list. Those ship with the renderer that produces them, exactly as a second renderer would ship
// its own.
//
// So string-debug provides the slots and this fills them; neither library depends on the other, and
// the app is what wires them together (see sandbox/demo_scene.cpp). The alternative — putting these
// in string-debug — would make the whole debug library depend on the forward renderer and stop a
// second renderer from reusing any of it.
//
// Holds the inspector's selection, so it is a class rather than free functions.
class RenderDebug
{
public:
    // Extra rows appended INSIDE the profiler HUD panel, under the generic per-pass GPU timings.
    // `stats` may be null (a scene with no geometry pass) — then nothing is added.
    void hud_rows(::string::ui::Ui& p, const MeshOverlayStats* stats) const;

    // The scene/draw inspector panel's contents: virtualized draw table + light list, with
    // hover/selection driving in-world debug-draw highlights.
    void inspector(::string::ui::Ui& p, const MeshOverlayStats* stats);

private:
    int selected_draw_ = -1;
};

}  // namespace string::render
