#pragma once

#include <string/render/debug_line_pass.hpp>
#include <string/render/geometry_pass.hpp>
#include <string/render/grid_2d_pass.hpp>
#include <string/render/post_pass.hpp>
#include <string/render/ui_background_pass.hpp>
#include <string/render/meshlet_data.hpp>
#include <string/render/render_cvars.hpp>
#include <string/render/ui_pass.hpp>
#include <string/client/ui_scene.hpp>

namespace sandbox
{

// The renderer's vocabulary, unqualified for the app.
//
// The app is a CONSUMER of `string::render` — it constructs that renderer's pass objects, declares
// them onto its own frame_graph, drives its UI pass, and reads per-frame stats for the debug
// surfaces. Re-exporting the handful of names it touches keeps call sites readable without a blanket
// `using namespace`, and the list doubles as an honest inventory of the app->renderer surface: if it
// grows, the app is reaching too far in.
//
// Brief 20: the nine wrapper *Pass names are gone. What the app names now are the COMPONENTS —
// plain objects it owns and declares — not pass subclasses the renderer drove.
using string::render::GpuMeshStats;
using string::render::InspectorDraw;
using string::render::InspectorLight;
using string::render::MeshOverlayStats;
using string::render::debug_line_pass;
using string::render::froxel_component;
using string::render::geometry_pass;
using string::render::grid_2d_pass;
using string::render::gtao_chain;
using string::render::ibl_component;
using string::render::post_pass;
using string::render::probe_gi_component;
using string::render::shadow_maps;
using string::render::shadertoy_pass;
using string::render::sky_component;
using string::render::sorted_transparency;
using string::render::ui_background_pass;
using string::client::UiAnchor;
using string::client::UiScene;
using string::client::UiSceneDriver;
using string::render::ui_pass;

// Two renderer CVars the app's debug UI drives: the draw-isolation bisection lever the inspector
// writes, and the UI screen selector the headless dump gate reads.
using string::render::cv_isolate_draw;
using string::render::cv_ui_screen;

}  // namespace sandbox
