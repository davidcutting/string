#include "demo_scene.hpp"

#include <filesystem>
#include <vector>

#include <string/core/layout.hpp>
#include "passes/grid_2d_pass.hpp"
#include "passes/geometry_pass.hpp"
#include "passes/ui_pass.hpp"

namespace sandbox
{
namespace
{

// Author the demo UI: a padded panel holding three boxes that exercise every shape and the
// border support — a sharp rectangle, a rounded rectangle with a border, and a bordered
// circle. Returns the positioned nodes (snapshotted so they outlive the builder), which the
// UIPass turns into GPU shapes.
std::vector<string::layout_node> build_demo_ui()
{
    using namespace string;

    element panel{};
    panel.color = { 30, 30, 46, 220 };
    panel.stroke_color = { 88, 91, 112, 255 };   // subtle surface border
    panel.stroke_width = 2;
    panel.radius = 16;
    panel.shape = shape::ROUNDED_RECTANGLE;
    panel.sizing = size_fit();   // shrink-wrap the children + padding

    // a) sharp rectangle, no border
    element rect_box{};
    rect_box.color = { 243, 139, 168, 255 };
    rect_box.shape = shape::RECTANGLE;
    rect_box.sizing = size_fixed(180, 60);

    // b) rounded rectangle with a border
    element rounded_box{};
    rounded_box.color = { 166, 227, 161, 255 };
    rounded_box.stroke_color = { 64, 120, 80, 255 };
    rounded_box.stroke_width = 3;
    rounded_box.radius = 12;
    rounded_box.shape = shape::ROUNDED_RECTANGLE;
    rounded_box.sizing = size_fixed(180, 60);

    // c) circle with a border
    element circle{};
    circle.color = { 137, 180, 250, 255 };
    circle.stroke_color = { 40, 60, 110, 255 };
    circle.stroke_width = 2;
    circle.shape = shape::CIRCLE;
    circle.sizing = size_fixed(60, 60);

    layout_builder b;
    b.begin(panel, format{ .padding = { 8, 8, 8, 8 }, .gap = 8, .direction = direction::VERTICAL })
         .add_element(rect_box)
         .add_element(rounded_box)
         .add_element(circle)
     .end();

    const auto nodes = b.nodes();
    return std::vector<layout_node>{ nodes.begin(), nodes.end() };
}

}  // namespace

String::RenderPlan build_demo_plan()
{
    String::RenderPlan plan;
    // Grid (background) then the Sponza scene on top, then the UI overlay last.
    plan.add<Grid2DPass>();
    plan.add<GeometryPass>(
        std::filesystem::path{ "assets/sponza/main/NewSponza_Main_glTF_003.gltf" });
    plan.add<UIPass>(build_demo_ui());
    return plan;
}

}  // namespace sandbox
