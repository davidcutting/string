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
// The app is a CONSUMER of `string::render` — it authors a RenderPlan out of that renderer's
// passes, drives its UI pass, and reads its per-frame stats for the debug surfaces. Re-exporting
// the handful of names it touches keeps the call sites readable without a blanket
// `using namespace`, and the list doubles as an honest inventory of the app→renderer surface:
// if it grows, the app is reaching too far in.
using string::render::GpuMeshStats;
using string::render::InspectorDraw;
using string::render::InspectorLight;
using string::render::MeshOverlayStats;
using string::render::DebugLinePass;
using string::render::FroxelPass;
using string::render::GeometryPass;
using string::render::GeometryPhase2Pass;
using string::render::GiPass;
using string::render::Grid2DPass;
using string::render::GtaoPass;
using string::render::HizBuildPass;
using string::render::IblPass;
using string::render::PostProcessPass;
using string::render::ShadowPass;
using string::render::SkyPass;
using string::render::TransparencyPass;
using string::render::UIBackgroundPass;
using string::client::UiAnchor;
using string::client::UiScene;
using string::client::UiSceneDriver;
using string::render::UIPass;

// Two renderer CVars the app's debug UI drives: the draw-isolation bisection lever the inspector
// writes, and the UI screen selector the headless dump gate reads.
using string::render::cv_isolate_draw;
using string::render::cv_ui_screen;

}  // namespace sandbox
