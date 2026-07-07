#pragma once

#include <string/vulkan/render_plan.hpp>

namespace sandbox
{

// Build the demo's render plan: the grid background, the viking-room mesh, and a small UI
// overlay. This is application content — the library provides the passes; the app decides
// which ones run, in what order, and with what data.
String::RenderPlan build_demo_plan();

}  // namespace sandbox
