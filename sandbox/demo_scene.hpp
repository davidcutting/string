#pragma once

#include <filesystem>

#include <string/vulkan/render_plan.hpp>

namespace sandbox
{

// Build the demo's render plan: the grid background, the viking-room mesh, and a small UI
// overlay (rects + text). This is application content — the library provides the passes; the app
// decides which ones run, in what order, and with what data. `resources_dir` locates the font
// asset (assets/fonts/) for the text overlay's atlas.
String::RenderPlan build_demo_plan(const std::filesystem::path& resources_dir);

}  // namespace sandbox
