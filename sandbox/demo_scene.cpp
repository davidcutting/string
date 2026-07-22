#include "demo_scene.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <string/core/font.hpp>
#include <string/core/layout.hpp>
#include "passes/geometry_pass.hpp"
#include "passes/ui_pass.hpp"

namespace sandbox
{
namespace
{

std::vector<std::uint8_t> read_file(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

// Author the whole demo UI every frame into ONE layout tree (UIPass draws its shapes and text from
// the same nodes): a rounded panel that holds a title, a live frame counter, a color swatch, a
// hover-highlight button, and an editable text field — the text lives inside the panel's layout.
// The builder arrives already inside UIPass's screen-filling root; we add a panel container.
//
// Interaction (ctx): `hovered`/`focused` are element id.hashes (the field/button carry ids); typing
// only mutates the field while it is focused. Modal: mouse-look capture = game (WASD/look to the
// camera); Esc frees the cursor for UI (hover, click-to-focus, type); a click on empty space
// re-captures. All dynamic strings live in the closure UIPass holds, so their views stay valid.
UIPass::Author make_ui_author(std::shared_ptr<const MeshOverlayStats> mesh_stats)
{
    return [frame = 0ull, counter = std::string{}, field = std::string{}, edit = std::string{},
            mesh_stats, stat_lines = std::vector<std::string>{}](
               string::layout_builder& b, const UIPass::UiContext& ctx) mutable {
        using namespace string;
        ++frame;
        counter = "frame " + std::to_string(frame);

        const std::uint64_t field_id = make_id("text_field").hash;
        const std::uint64_t button_id = make_id("hover_button").hash;
        const bool field_focused = ctx.focused == field_id;

        // The field only consumes text input while it is focused (so flying with WASD in game mode
        // — where nothing is focused — never types into it).
        if (field_focused)
        {
            field += ctx.input.typed_text();
            if (ctx.input.key_pressed(String::KeyCode::BACKSPACE) && !field.empty())
            {
                field.pop_back();
            }
        }
        edit = "> " + field + (field_focused ? "_" : "");

        element panel{};
        panel.color = { 30, 30, 46, 235 };
        panel.stroke_color = { 88, 91, 112, 255 };
        panel.stroke_width = 2;
        panel.radius = 16;
        panel.shape = shape::ROUNDED_RECTANGLE;
        panel.sizing = size_fit();  // shrink-wrap to its children + padding

        element title{};
        title.color = { 205, 214, 244, 255 };
        title.sizing = size_fit();

        element counter_el{};
        counter_el.color = { 166, 227, 161, 255 };
        counter_el.sizing = size_fit();

        // A color swatch (a plain shape node) to show shapes + text share the one tree.
        element swatch{};
        swatch.color = { 137, 180, 250, 255 };
        swatch.radius = 8;
        swatch.shape = shape::ROUNDED_RECTANGLE;
        swatch.sizing = size_fixed(220, 28);

        element button{};
        button.id = make_id("hover_button");
        button.sizing = { fit(220), fit() };  // widen the hit area so it's easy to hover
        const bool hot = ctx.hovered == button_id;
        button.color = hot ? color{ 243, 139, 168, 255 } : color{ 147, 153, 178, 255 };

        element field_el{};
        field_el.id = make_id("text_field");
        field_el.sizing = { fit(220), fit() };
        field_el.color = field_focused ? color{ 249, 226, 175, 255 } : color{ 166, 173, 200, 255 };

        b.begin(panel, format{ .padding = { 12, 12, 12, 12 }, .gap = 8, .direction = direction::VERTICAL })
             .add_text(title, "Hello, String!", 32)
             .add_text(counter_el, counter, 32)
             .add_element(swatch)
             .add_text(button, hot ? "hovering!" : "hover me", 32)
             .add_text(field_el, edit, 32)
         .end();

        // --- Brief 03: meshlet culling-stats overlay (GPU counters, read back one frame late) ---
        if (mesh_stats)
        {
            const MeshOverlayStats& m = *mesh_stats;
            const GpuMeshStats& s = m.stats;
            static const char* view_names[] = { "off", "meshlet-id", "LOD", "occlusion" };
            auto pct = [](uint32_t num, uint32_t den) {
                return den == 0 ? std::string("--") : std::to_string(num * 100 / den) + "%";
            };
            stat_lines.clear();
            stat_lines.push_back("HiZ: " + std::string(m.hiz_enabled ? "on" : "off") +
                                 "   view: " + view_names[m.debug_view & 3] +
                                 (m.crowd_enabled ? "   [CROWD]" : ""));
            stat_lines.push_back("meshlets: " + std::to_string(s.meshlets_total) +
                                 " / " + std::to_string(m.total_meshlets));
            stat_lines.push_back("frustum: " + std::to_string(s.after_frustum) +
                                 " (" + pct(s.after_frustum, s.meshlets_total) + ")");
            stat_lines.push_back("cone:    " + std::to_string(s.after_cone) +
                                 " (" + pct(s.after_cone, s.meshlets_total) + ")");
            stat_lines.push_back("hiz/drawn: " + std::to_string(s.after_hiz) +
                                 " (" + pct(s.after_hiz, s.meshlets_total) + ")");
            stat_lines.push_back("LOD draws: " + std::to_string(s.draws_per_lod[0]) + "/" +
                                 std::to_string(s.draws_per_lod[1]) + "/" +
                                 std::to_string(s.draws_per_lod[2]) + "/" +
                                 std::to_string(s.draws_per_lod[3]));

            element stats_panel{};
            stats_panel.color = { 20, 22, 34, 235 };
            stats_panel.stroke_color = { 88, 91, 112, 255 };
            stats_panel.stroke_width = 2;
            stats_panel.radius = 12;
            stats_panel.shape = shape::ROUNDED_RECTANGLE;
            stats_panel.sizing = size_fit();

            b.begin(stats_panel, format{ .padding = { 10, 10, 10, 10 }, .gap = 4, .direction = direction::VERTICAL });
            for (const std::string& line : stat_lines)
            {
                element row{};
                row.color = { 205, 214, 244, 255 };
                row.sizing = size_fit();
                b.add_text(row, line, 20);
            }
            b.end();
        }
    };
}

}  // namespace

String::RenderPlan build_demo_plan(const std::filesystem::path& resources_dir)
{
    // The UI's SDF atlas is baked once, up front and shared: UIPass's per-frame layout measures
    // against it and draws text from it.
    const std::vector<std::uint8_t> ttf = read_file(resources_dir / "assets/fonts/DejaVuSans.ttf");
    auto atlas = std::make_shared<string::font_atlas>(string::build_font_atlas(ttf, 32.0f));

    // Shared meshlet culling stats: the GeometryPass fills it each frame, the UI author reads it
    // (both run on the render thread; the shared_ptr is just the plumbing seam between passes).
    auto mesh_stats = std::make_shared<MeshOverlayStats>();

    String::RenderPlan plan;
    // The Sponza scene (which now draws its own procedural sky background), then the single UI
    // overlay (shapes + text) last. The debug grid is gone — the sky fills the background.
    plan.add<GeometryPass>(
        std::filesystem::path{ "assets/sponza/main/NewSponza_Main_glTF_003.gltf" }, mesh_stats);
    plan.add<UIPass>(atlas, make_ui_author(mesh_stats));
    return plan;
}

}  // namespace sandbox
