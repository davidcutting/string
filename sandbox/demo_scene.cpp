#include "demo_scene.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <algorithm>
#include <chrono>

#include <string/core/dynamic_font.hpp>
#include <string/core/font.hpp>
#include <string/core/layout.hpp>
#include <string/core/logger.hpp>
#include "debug_cvars.hpp"
#include "passes/debug_line_pass.hpp"
#include "passes/geometry_pass.hpp"
#include "passes/ui_background_pass.hpp"
#include "passes/ui_pass.hpp"
#include "passes/ui_scene.hpp"
#include "ui/debug_panels.hpp"
#include "ui/screens.hpp"

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
UIPass::Author make_ui_author(std::shared_ptr<const MeshOverlayStats> mesh_stats,
                              std::size_t nameplate_stress)
{
    return [mesh_stats,
            np_driver = UiSceneDriver(static_cast<std::uint32_t>(nameplate_stress)),
            np_scene = UiScene{}, np_scratch = ui::ScreenScratch{}, nameplate_stress,
            panels = ui::DebugPanels{}](
               string::layout_builder& b, const UIPass::UiContext& ctx) mutable {
        // Optional synthetic nameplate stress OVER the Sponza scene (STRING_UI_NAMEPLATES): the same
        // orbit driver as the ui-dev scene, so UI cost on top of real geometry is measurable (brief
        // 05 perf: report frametime with the stress in BOTH scenes).
        if (nameplate_stress > 0)
        {
            np_driver.update(ctx.delta_time, 1920, 1080, np_scene);
            ui::author_nameplates(b, np_scene, np_scratch, nameplate_stress);
        }

        // Brief 06: debug surfaces (console + profiler HUD + inspector), toggled at runtime
        // (grave = console, F2 = HUD, F3 = inspector), off by default so headless captures stay
        // clean unless asked for. The old "Hello, String!" demo panel was removed from this scene
        // (user: placeholder clutter that the console overlapped) — the widget/interaction
        // showcase lives in the ui-dev scene (STRING_SCENE=ui).
        panels.update_and_author(b, ctx, mesh_stats.get());
    };
}

// The ui-dev sandbox author (brief 05): synthetic world-anchored nameplates + a status panel, over
// the flat gradient background (no geometry). Reads the shared UiScene the background pass drives.
// This is the permanent UI sandbox — STRING_SCENE=ui ./run.sh. `mesh_stats` is intentionally absent
// (the overlay tolerates a null MeshOverlayStats); the demo panel from make_ui_author is reused so
// hover/focus/text-field interaction is still exercised here.
UIPass::Author make_ui_dev_author(std::shared_ptr<UiScene> scene, std::size_t nameplate_budget,
                                  std::string screen)
{
    return [scene, nameplate_budget, screen, scratch = ui::ScreenScratch{}, motion = ui::Motion{},
            state = ui::ScreenState{}, panels = ui::DebugPanels{}](
               string::layout_builder& b, const UIPass::UiContext& ctx) mutable {
        const ui::Interaction it = ui::Interaction::from(ctx);
        motion.begin_frame(ctx.delta_time);
        state.tick(ctx.delta_time);
        scratch.lines.clear();

        const bool all = screen == "all";
        std::size_t np_count = 0;

        if (all || screen == "nameplates")
        {
            ui::author_nameplates(b, *scene, scratch, nameplate_budget);
            np_count = nameplate_budget == 0 ? scene->anchors.size()
                                             : std::min(nameplate_budget, scene->anchors.size());
        }
        if (all || screen == "inventory")
            ui::author_inventory(b, motion, it, scratch);
        if (all || screen == "actionbar")
            ui::author_actionbar(b, motion, it, state, scratch);
        if (all || screen == "chat")
            ui::author_chat_pings(b, motion, it, *scene, state, scratch);

        ui::author_status_panel(b, *scene, scratch, screen, np_count);
        motion.end_frame();

        // Brief 06 debug surfaces work in the ui-dev sandbox too (console/HUD; inspector shows "no
        // scene" since there's no geometry). Lets tooling be iterated in STRING_SCENE=ui.
        panels.update_and_author(b, ctx, nullptr);
    };
}

}  // namespace

String::RenderPlan build_demo_plan(const std::filesystem::path& resources_dir)
{
    // The UI's SDF atlas is baked once, up front and shared: UIPass's per-frame layout measures
    // against it and draws text from it.
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<std::uint8_t> ttf = read_file(resources_dir / "assets/fonts/DejaVuSans.ttf");
    // Dynamic (grow-on-demand, Unicode) SDF atlas: glyphs rasterise on first sight, baked at a large
    // reference size (crisp small text), player-name-safe. Replaces the baked ASCII atlas (brief 05).
    auto atlas = std::make_shared<string::dynamic_font_atlas>(ttf);

    const std::string scene_sel = cv_scene().get();
    String::RenderPlan plan;

    if (scene_sel == "ui")
    {
        // UI-dev sandbox: background (+ orbit camera + synthetic anchors) then the UI overlay. NO
        // GeometryPass — startup is near-instant (no glTF load / meshlet build / texture stream).
        auto ui_scene = std::make_shared<UiScene>();
        int np = cv_ui_nameplates().get();
        const std::string screen = cv_ui_screen().get();
        // Default a visible handful of anchors so nameplates show without opting into the stress.
        const std::uint32_t anchor_count = np > 0 ? static_cast<std::uint32_t>(np) : 24u;
        const std::size_t np_budget = np > 0 ? static_cast<std::size_t>(np) : 0;  // 0 = draw all

        plan.add<UIBackgroundPass>(ui_scene, anchor_count);
        plan.add<UIPass>(atlas, make_ui_dev_author(ui_scene, np_budget, screen));

        const auto t1 = std::chrono::steady_clock::now();
        STRING_LOG_INFO("ui-dev scene: plan built in {} ms ({} synthetic anchors, screen '{}')",
                        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
                        anchor_count, screen);
        return plan;
    }

    if (scene_sel == "lookdev")
    {
        // Brief 07: the standing material-probe scene — a roughness x metallic sphere grid plus a
        // white/mirror pair, generated in-process (no glTF load; near-instant startup). Same
        // GeometryPass, so sun/TOD scrub keys, the furnace CVar, IBL, shadows and all capture
        // levers work identically. Debug lines + UI ride along for the console/HUD.
        auto mesh_stats = std::make_shared<MeshOverlayStats>();
        plan.add<GeometryPass>(std::vector<std::filesystem::path>{}, mesh_stats, /*lookdev=*/true);
        plan.add<DebugLinePass>(mesh_stats);
        plan.add<UIPass>(atlas, make_ui_author(mesh_stats, 0));
        const auto t1 = std::chrono::steady_clock::now();
        STRING_LOG_INFO("lookdev scene: plan built in {} ms",
                        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        return plan;
    }

    // Default: the full Sponza scene (which draws its own procedural sky background), then the single
    // UI overlay last. Main + curtains + ivy share one world space and merge into a single draw set;
    // the curtains exercise the brief-04 two-sided/blend material paths on real content.
    auto mesh_stats = std::make_shared<MeshOverlayStats>();
    plan.add<GeometryPass>(
        std::vector<std::filesystem::path>{
            "assets/sponza/main/NewSponza_Main_glTF_003.gltf",
            "assets/sponza/curtains/NewSponza_Curtains_glTF.gltf",
            "assets/sponza/ivy/NewSponza_IvyGrowth_glTF.gltf",
        },
        mesh_stats);
    // Brief 06: immediate-mode debug lines (inspector AABB highlights, debug-draw test patterns)
    // draw into the scene color after geometry, before the UI overlay. Shares geometry's camera
    // snapshot via mesh_stats.
    plan.add<DebugLinePass>(mesh_stats);
    const int np_stress = std::max(0, cv_ui_nameplates().get());
    plan.add<UIPass>(atlas, make_ui_author(mesh_stats, static_cast<std::size_t>(np_stress)));
    return plan;
}

}  // namespace sandbox
