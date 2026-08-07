#pragma once

#include <filesystem>

#include <string/application.hpp>

namespace sandbox
{

// Build the demo's scene: one callback that constructs the app's pass objects against the ready
// graph, declares them, and returns the per-frame tick.
//
// Brief 20: this used to return a RenderPlan the renderer drove. The application owns its graph now
// — what it declares is its own, and only what is DERIVED from those declarations (acquire, resize,
// queue placement, present) stays behind the renderer. `resources_dir` locates the font asset for
// the UI overlay's atlas.
String::Application::scene_fn build_demo_scene(const std::filesystem::path& resources_dir);

}  // namespace sandbox
