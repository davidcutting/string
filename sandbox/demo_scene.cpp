#include "demo_scene.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <algorithm>
#include <chrono>

#include <string/ui/dynamic_font.hpp>
#include <string/ui/font.hpp>
#include <string/ui/layout.hpp>
#include <string/core/logger.hpp>
#include <string/vulkan/frame_graph.hpp>
#include <string/vulkan/scene_registry.hpp>
#include "debug_cvars.hpp"
#include <string/render/debug_line_pass.hpp>
#include <string/render/geometry_pass.hpp>
#include <string/render/post_pass.hpp>
#include <string/render/ui_background_pass.hpp>
#include <string/render/ui_pass.hpp>
#include "render_types.hpp"
#include <string/debug/panels.hpp>
#include <string/client/screens.hpp>
#include <string/render/render_debug.hpp>

namespace sandbox
{

// The app is the only place that knows about ALL the libraries — it is what wires the renderer's
// debug panels into the debug shell, and the client's screens into the UI pass. Spelling the
// three namespaces short keeps the authoring code readable.
namespace client = ::string::client;
namespace debug = ::string::debug;
namespace ui = ::string::ui;
}

namespace sandbox
{
namespace
{

// Brief 11 endgame: author a stateful pass's standard runtime surface onto the graph — its live
// per-frame usages + record + optional prepass-compute bodies (both delegate to the pass object).
// The caller adds nature flags fluently (.computeOnly()/.prepass()/.async()/.toggle()) then .finish().
String::PassSpec author_pass(String::FrameGraph& fg, String::Pass* p)
{
    return fg.pass(std::string(p->debug_name()))
        // Brief 16: declare the pass's usages via a getter (retires usagesFrom's raw member pointer);
        // the usages carry logical handles, the executor resolves the per-frame physical.
        .usages([p]() -> const std::vector<String::ResourceUsage>& { return p->usages; })
        // Brief 16 M1: the executor hands a pass_context; the legacy Pass interface still takes
        // (command_recorder&, frame), so unpack ctx here. Handle-native passes take ctx directly.
        .record([p](string::gpu::pass_context& ctx) { p->record(ctx.rec, ctx.frame_slot); })
        .prepassCompute([p](string::gpu::pass_context& ctx) { return p->record_compute(ctx.rec, ctx.frame_slot); });
}

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
// The `Ui` is handed in as a shared slot rather than captured privately so the POST-LAYOUT hook can
// reach the same instance: `Ui::observe()` caches the boxes widgets asked to measure, and sizes only
// exist after layout. Same seam, and same reason, as `Workspace::observe`.
using SharedUi = std::shared_ptr<std::optional<ui::Ui>>;
// The debug shell is shared for the SAME reason: it owns a second `Ui`, and that one needs
// The debug shell no longer owns a Ui, so it is not passed here — see DebugPanels::update_and_author.
using SharedPanels = std::shared_ptr<debug::DebugPanels>;

// Post-layout hook for an author built around `fluent`. Null-safe on the first frame, when the Ui has
// not been constructed yet (it needs the builder and interaction references the author receives).
//
// ONE Ui means one `observe()`. This used to call a second one for the debug shell, and forgetting
// that call is exactly how the DAG panel's scroll extent came to be permanently zero.
// Drains the Ui's deferred surfaces one at a time; the pass closes each one's layout because only
// it holds the measurer. Null-safe before the Ui exists, like the observer below.
UIPass::DeferredAuthor make_deferred_author(SharedUi fluent)
{
    return [fluent] { return *fluent && (*fluent)->emit_next_deferred(); };
}

UIPass::PostLayout make_ui_observer(SharedUi fluent,
                                    std::shared_ptr<string::ui::Workspace> ws = nullptr)
{
    return [fluent, ws](const string::layout_builder& b) {
        if (ws) ws->observe(b);
        if (*fluent) (*fluent)->observe();
    };
}

UIPass::Author make_ui_author(std::shared_ptr<const MeshOverlayStats> mesh_stats,
                              SharedPanels panels,
                              std::size_t nameplate_stress, SharedUi fluent)
{
    return [mesh_stats,
            np_driver = UiSceneDriver(static_cast<std::uint32_t>(nameplate_stress)),
            np_scene = UiScene{}, motion = ui::Motion{}, ui_panels = string::ui::PanelStore{},
            fluent = std::move(fluent), nameplate_stress,
            panels, render_dbg = ::string::render::RenderDebug{}](
               string::layout_builder& b, const UIPass::UiContext& ctx) mutable {
        // ONE Ui per frame, always constructed — the debug shell authors into this same one, so its
        // panels share the app's PanelStore (and therefore one z order) and its per-frame hand-offs
        // are forwarded once, below, rather than twice from two places.
        //
        // Closure-lived, not stack-lived: the arena it owns backs every text view for the rest of
        // the frame (see the lifetime note on Ui).
        if (!*fluent) fluent->emplace(b, ctx.ui, motion, client::theme(), ui_panels);
        ui::Ui& u = **fluent;
        u.begin_frame();

        // Optional synthetic nameplate stress OVER the Sponza scene (STRING_UI_NAMEPLATES): the same
        // orbit driver as the ui-dev scene, so UI cost on top of real geometry is measurable (brief
        // 05 perf: report frametime with the stress in BOTH scenes).
        if (nameplate_stress > 0)
        {
            np_driver.update(ctx.ui.dt, 1920, 1080, np_scene);
            client::author_nameplates(u, np_scene, nameplate_stress);
        }

        // Brief 06: debug surfaces (console + profiler HUD + inspector), toggled at runtime
        // (grave = console, F2 = HUD, F3 = inspector), off by default so headless captures stay
        // clean unless asked for. The old "Hello, String!" demo panel was removed from this scene
        // (user: placeholder clutter that the console overlapped) — the widget/interaction
        // showcase lives in the ui-dev scene (STRING_SCENE=ui).
        // The APP is what joins the two libraries: the debug shell owns the panel/toggle machinery
        // and knows nothing about renderers; the renderer supplies the numbers. Rebinding each
        // frame keeps `mesh_stats` (a shared_ptr the pass refreshes) current without the shell
        // ever holding a renderer type.
        panels->set_hud_extra([&](ui::Ui& p) { render_dbg.hud_rows(p, mesh_stats.get()); });
        panels->set_inspector([&](ui::Ui& p) { render_dbg.inspector(p, mesh_stats.get()); });
        panels->update_and_author(u, ctx.input, ctx.input_map);

        u.end_frame();

        // The Ui's per-frame hand-offs, forwarded ONCE now that there is one Ui. Forgetting one used
        // to be silent, and did in fact happen twice.
        ctx.input.set_text_capture(u.wants_text_capture());
        if (!u.clipboard_write().empty())
            ctx.input.request_clipboard_write(std::string(u.clipboard_write()));
    };
}

// The ui-dev sandbox author (brief 05): synthetic world-anchored nameplates + a status panel, over
// the flat gradient background (no geometry). The AUTHOR drives the orbit camera + synthetic
// anchors, exactly as the Sponza author already did — a demo camera is app logic, not something a
// render pass should own (that ownership was the only reason string-client needed the renderer).
// This is the permanent UI sandbox — STRING_SCENE=ui ./run.sh. `mesh_stats` is intentionally absent
// (the overlay tolerates a null MeshOverlayStats); the demo panel from make_ui_author is reused so
// hover/focus/text-field interaction is still exercised here.
UIPass::Author make_ui_dev_author(std::shared_ptr<UiScene> scene, std::uint32_t anchor_count,
                                  SharedPanels panels,
                                  std::size_t nameplate_budget,
                                  std::string screen,
                                  std::shared_ptr<string::ui::Workspace> ws, SharedUi fluent)
{
    return [scene, nameplate_budget, screen, ws, motion = ui::Motion{}, ui_panels = string::ui::PanelStore{},
            driver = client::UiSceneDriver(anchor_count),
            fluent = std::move(fluent),
            state = client::ScreenState{}, panels,
            render_dbg = ::string::render::RenderDebug{}](
               string::layout_builder& b, const UIPass::UiContext& ctx) mutable {
        // The Ui lives in the CLOSURE, not on the stack: it owns the frame string arena and the
        // layout tree holds non-owning views into it, so it must outlive authoring — through layout
        // and record. Constructed on the first frame (the builder and interaction it binds are
        // stable members of UIPass, so the references stay valid for the run).
        if (!*fluent) fluent->emplace(b, ctx.ui, motion, client::theme(), ui_panels);
        ui::Ui& u = **fluent;
        u.begin_frame();
        state.tick(ctx.ui.dt);

        // Advance the orbit camera + write the projection into the shared UiScene. This used to run
        // in UIBackgroundPass::update; it lands at the same point in the same frame with the same
        // dt and screen size, so the projected nameplate positions are unchanged.
        driver.update(ctx.ui.dt, ctx.ui.screen.width, ctx.ui.screen.height, *scene);

        const bool all = screen == "all";
        std::size_t np_count = 0;

        if (all || screen == "nameplates")
        {
            client::author_nameplates(u, *scene, nameplate_budget);
            np_count = nameplate_budget == 0 ? scene->anchors.size()
                                             : std::min(nameplate_budget, scene->anchors.size());
        }
        if (all || screen == "inventory")
            client::author_inventory(u);
        if (all || screen == "actionbar")
            client::author_actionbar(u, state);
        if (all || screen == "chat")
            client::author_chat_pings(u, *scene, state);
        // Not in "all": keeping these opt-in leaves the five brief-05 dump baselines untouched.
        if (screen == "panels")
            client::author_panels(u, state);
        if (screen == "workspace" && ws)
            client::author_workspace(u, *ws, state);
        if (screen == "widgets")
            client::author_widgets(u, state);

        client::author_status_panel(u, *scene, state, screen, np_count);

        // Brief 06 debug surfaces work in the ui-dev sandbox too (console/HUD; inspector shows "no
        // scene" since there's no geometry). Lets tooling be iterated in STRING_SCENE=ui.
        // The renderer's panels are registered here too, with no stats to read: the inspector then
        // reports "no scene loaded" the way it always did, rather than "no inspector registered"
        // — an empty scene is a normal state, a missing registration is a wiring bug.
        panels->set_hud_extra([&](ui::Ui& p) { render_dbg.hud_rows(p, nullptr); });
        panels->set_inspector([&](ui::Ui& p) { render_dbg.inspector(p, nullptr); });
        panels->update_and_author(u, ctx.input, ctx.input_map);

        u.end_frame();

        // The Ui's per-frame hand-offs, forwarded ONCE. The engine REQUESTS text capture (a focused
        // field wants the keystrokes) and a clipboard write; the host grants them, because both are
        // platform decisions — the same split as the game/UI mode requests.
        ctx.input.set_text_capture(u.wants_text_capture());
        if (!u.clipboard_write().empty())
            ctx.input.request_clipboard_write(std::string(u.clipboard_write()));
    };
}


// Build the full geometry scene Setup around an already-constructed GeometryPass: the meshlet
// subsystem's companion passes (froxel/ibl/gtao/sky/hiz/phase2) + debug lines + UI + post, plus the
// fluent author that declares each pass's nature onto the graph. Shared by the lookdev + Sponza scenes.
String::RenderPlan::Setup make_geometry_setup(
    String::engine_context& ctx, std::unique_ptr<GeometryPass> geo,
    std::shared_ptr<MeshOverlayStats> mesh_stats,
    std::shared_ptr<string::dynamic_font_atlas> atlas, std::size_t nameplate_stress)
{
    GeometryPass* gp = geo.get();
    auto froxel = std::make_unique<FroxelPass>(ctx, gp->scene());
    auto ibl    = std::make_unique<IblPass>(ctx, gp->scene());
    auto gtao   = std::make_unique<GtaoPass>(gp);
    auto gi     = std::make_unique<GiPass>(gp);
    auto sky    = std::make_unique<SkyPass>(ctx, gp->scene());
    auto hiz    = std::make_unique<HizBuildPass>(gp);
    auto phase2 = std::make_unique<GeometryPhase2Pass>(gp);
    auto transp = std::make_unique<TransparencyPass>(gp);
    auto shadow = std::make_unique<ShadowPass>(gp);
    auto dbg    = std::make_unique<DebugLinePass>(ctx, mesh_stats);
    auto ui_slot = std::make_shared<std::optional<ui::Ui>>();
    auto dbg_panels = std::make_shared<debug::DebugPanels>();
    auto ui     = std::make_unique<UIPass>(ctx, atlas,
                                           make_ui_author(mesh_stats, dbg_panels, nameplate_stress, ui_slot),
                                           make_ui_observer(ui_slot),
                                           make_deferred_author(ui_slot));
    auto post   = std::make_unique<PostProcessPass>(ctx);

    FroxelPass* fx = froxel.get();
    String::Pass* ib = ibl.get();  String::Pass* gt = gtao.get(); String::Pass* sk = sky.get();
    String::Pass* gi_p = gi.get();
    String::Pass* hz = hiz.get();  String::Pass* p2 = phase2.get();  String::Pass* sh = shadow.get();
    String::Pass* tr = transp.get();
    String::Pass* db = dbg.get();  String::Pass* uip = ui.get();   String::Pass* po = post.get();

    // Ownership order == the per-frame update()/resize() LIFECYCLE order, which is load-bearing:
    // FroxelPass::update() allocates the froxel index buffer, whose device address GeometryPass::update()
    // then bakes into SceneData — so froxel (and the other producers) MUST update before geometry, or
    // frame 0 bakes a null froxel address and the lit shader faults the GPU (DEVICE_LOST). This matches
    // the graph author order below; it is NOT the same concern as the toposort. (Producers first.)
    String::RenderPlan::Setup s;
    s.passes.push_back(std::move(froxel));
    s.passes.push_back(std::move(ibl));
    s.passes.push_back(std::move(gtao));
    s.passes.push_back(std::move(gi));
    s.passes.push_back(std::move(sky));
    s.passes.push_back(std::move(geo));
    s.passes.push_back(std::move(hiz));
    s.passes.push_back(std::move(phase2));
    s.passes.push_back(std::move(transp));
    s.passes.push_back(std::move(shadow));
    s.passes.push_back(std::move(dbg));
    s.passes.push_back(std::move(ui));
    s.passes.push_back(std::move(post));

    // The render graph, authored fluently — order = record order (the toposort preserves it). Nature is
    // declared HERE, not via Pass virtuals: froxel is the async light-binning chain; ibl/gtao are
    // frame-top prepass compute; hiz.build + post are compute-only; geometry.phase1 is toggleable.
    s.author = [fx, ib, gt, gi_p, sk, gp, hz, p2, tr, sh, db, uip, po](String::FrameGraph& fg) {
        author_pass(fg, fx).computeOnly()
            .async([fx] { return fx->async_has_work(); },
                   [fx](string::gpu::pass_context& ctx) { fx->record_async_compute(ctx.rec, ctx.frame_slot); },
                   [fx]() -> const std::vector<String::ResourceUsage>& { return fx->async_usages; }).finish();
        author_pass(fg, ib).prepass().finish();
        // gtao / gi: prepass seams, toggleable through their feature cvars (r.gtao.enabled / r.gi) —
        // off drops the pass AND SceneData already degrades (gtao_slot invalid / probe_gi 0).
        author_pass(fg, gt).prepass().toggle([gt] { return gt->is_enabled(); }).finish();
        // gi.probe: prepass compute seam over GeometryPass. Authored after ibl (relight reads this
        // frame's SH) and before geometry+shadow (relight's shadow-map reads precede the shadow depth
        // writes — the WAR execution edge). prepass() = frame-top compute, forms no render group.
        author_pass(fg, gi_p).prepass().toggle([gi_p] { return gi_p->is_enabled(); }).finish();
        // sky: r.pass.sky drops the background draw -> the MSAA clear shows behind the lit scene.
        author_pass(fg, sk).toggle([sk] { return sk->is_enabled(); }).finish();
        // geometry.phase1 PRODUCES hz.depth — declared here so the PLAN knows it.
        //
        // The write itself is the group's MSAA depth MIN-resolve at EndRendering, which the graph
        // cannot observe (it is an attachment property, not a recorded command). Sync was already
        // correct: the renderer seeds the tracker with the post-resolve state, and hiz.build's
        // SampledRead derives its barrier from that. What was missing is the DEPENDENCY: with no
        // declared producer, `hz.depth` had first_writer = none and there was no geometry -> hiz.build
        // edge, so their order rested on authoring order and the toposort's tie-break rather than on
        // anything the planner could enforce. The M3 DAG made that visible — hiz.build sat at depth 0
        // with nothing feeding it.
        //
        // This goes through `use()`, which appends to the pass's DECLARED plan lists only. It does
        // NOT touch `Pass::usages`, which is what barrier derivation reads live — so this buys the
        // edge, the toposort constraint and the lifetime span, and changes no barrier. The separate
        // record-time `Access::DepthResolve` marker still names the resolve target for the group
        // loop; it stays a non-write because the group loop scans for it by identity, and it is
        // appended after compile so the planner never sees it anyway.
        {
            String::PassSpec geo_spec = author_pass(fg, gp).toggle([gp] { return gp->is_enabled(); });
            if (gp->hiz_depth_ring().valid())
                geo_spec.use(String::ResourceUsage{
                    .access = String::Access::DepthWrite,
                    .stage = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    .img = gp->hiz_depth_ring() });
            geo_spec.finish();
        }
        author_pass(fg, hz).computeOnly().finish();
        author_pass(fg, p2).finish();
        // transparency: joins phase2's reopened MSAA group as its LAST draw (same COLOR_TARGET, authored
        // immediately after phase2 so it's consecutive — it must come BEFORE the prepass shadow seam or
        // shadow's non-color usage would split the group between phase2 and transparency). r.pass.transparency
        // drops it -> opaque geometry only.
        author_pass(fg, tr).toggle([tr] { return tr->is_enabled(); }).finish();
        // shadow.cascades: prepass compute seam over GeometryPass. Authored AFTER phase2 so its
        // record_compute is ordered after geometry.phase1's (draw-cull/expand produced the per-cascade
        // worklists it draws) while its record() no-ops — it must NOT sit between phase1 and phase2 or it
        // would split the reopened MSAA group. prepass() = frame-top compute, forms no render group.
        // r.pass.shadow drops it; SceneData cascade_count 0 (set in GeometryPass::update off the same
        // cvar) makes the scene render unshadowed.
        author_pass(fg, sh).prepass().toggle([sh] { return sh->is_enabled(); }).finish();
        author_pass(fg, db).finish();
        author_pass(fg, uip).finish();
        author_pass(fg, po).computeOnly().finish();
    };
    return s;
}

}  // namespace

String::RenderPlan build_demo_plan(const std::filesystem::path& resources_dir)
{
    // The UI's SDF atlas is baked once, up front and shared: UIPass's per-frame layout measures
    // against it and draws text from it.
    const std::vector<std::uint8_t> ttf = read_file(resources_dir / "assets/fonts/DejaVuSans.ttf");
    // Dynamic (grow-on-demand, Unicode) SDF atlas: glyphs rasterise on first sight, baked at a large
    // reference size (crisp small text), player-name-safe. Replaces the baked ASCII atlas (brief 05).
    auto atlas = std::make_shared<string::dynamic_font_atlas>(ttf);

    String::SceneRegistry& registry = String::SceneRegistry::instance();

    // Registration order is the order the scene panel lists them in.
    {
        // UI-dev sandbox: background (+ orbit camera + synthetic anchors) then the UI overlay. NO
        // GeometryPass — startup is near-instant (no glTF load / meshlet build / texture stream).
        auto ui_scene = std::make_shared<UiScene>();
        int np = cv_ui_nameplates().get();
        const std::string screen = cv_ui_screen().get();
        // Default a visible handful of anchors so nameplates show without opting into the stress.
        const std::uint32_t anchor_count = np > 0 ? static_cast<std::uint32_t>(np) : 24u;
        const std::size_t np_budget = np > 0 ? static_cast<std::size_t>(np) : 0;  // 0 = draw all

        registry.add("ui", "UI sandbox — no geometry, instant start",
                     [atlas, ui_scene, anchor_count, np_budget, screen](String::engine_context& ctx)
                       -> String::RenderPlan::Setup {
            auto bg = std::make_unique<UIBackgroundPass>(ctx);
            // Shared between the author and the post-layout hook: the workspace is authored before
            // layout and observed after it, and both need the same instance.
            auto ws = std::make_shared<string::ui::Workspace>();
            if (screen == "workspace")
                client::seed_workspace(*ws);
            auto ui_slot = std::make_shared<std::optional<ui::Ui>>();
            auto dbg_panels = std::make_shared<debug::DebugPanels>();
            auto ui = std::make_unique<UIPass>(
                ctx, atlas,
                make_ui_dev_author(ui_scene, anchor_count, dbg_panels, np_budget, screen, ws, ui_slot),
                make_ui_observer(ui_slot, ws),
                make_deferred_author(ui_slot));
            String::Pass* bgp = bg.get();
            String::Pass* uip = ui.get();
            String::RenderPlan::Setup s;
            s.passes.push_back(std::move(bg));
            s.passes.push_back(std::move(ui));
            s.author = [bgp, uip](String::FrameGraph& fg) {
                author_pass(fg, bgp).finish();   // background (+ synthetic anchors) into the scene target
                author_pass(fg, uip).finish();   // UI overlay
            };
            return s;
        });
    }

    {
        // Brief 07: the standing material-probe scene — a roughness x metallic sphere grid plus a
        // white/mirror pair, generated in-process (no glTF load; near-instant startup). Same
        // GeometryPass, so sun/TOD scrub keys, the furnace CVar, IBL, shadows and all capture
        // levers work identically. Debug lines + UI ride along for the console/HUD.
        // Brief 09: the lookdev scene PINS MANUAL exposure by default (it is the EV100/material
        // calibration reference — auto-metering a sphere grid over grey would defeat that).
        // setenv with overwrite=0: an explicit STRING_EXPOSURE_AUTO from the user still wins.
        setenv("STRING_EXPOSURE_AUTO", "0", 0);
        registry.add("lookdev", "Material probe grid — roughness x metallic",
                     [atlas](String::engine_context& ctx) -> String::RenderPlan::Setup {
            auto mesh_stats = std::make_shared<MeshOverlayStats>();
            auto geo = std::make_unique<GeometryPass>(ctx, std::vector<std::filesystem::path>{},
                                                      mesh_stats, /*lookdev=*/true);
            return make_geometry_setup(ctx, std::move(geo), mesh_stats, atlas, /*nameplate_stress=*/0);
        });
    }

    // Default: the full Sponza scene (which draws its own procedural sky background), then debug lines +
    // the UI overlay + post. Main + curtains + ivy share one world space and merge into a single draw
    // set; the curtains exercise the brief-04 two-sided/blend material paths on real content.
    const int np_stress = std::max(0, cv_ui_nameplates().get());
    registry.add("sponza", "Sponza — full scene, meshlets, CSM, GI",
                 [atlas, np_stress](String::engine_context& ctx) -> String::RenderPlan::Setup {
        auto mesh_stats = std::make_shared<MeshOverlayStats>();
        auto geo = std::make_unique<GeometryPass>(ctx,
            std::vector<std::filesystem::path>{
                "assets/sponza/main/NewSponza_Main_glTF_003.gltf",
                "assets/sponza/curtains/NewSponza_Curtains_glTF.gltf",
                "assets/sponza/ivy/NewSponza_IvyGrowth_glTF.gltf",
            },
            mesh_stats);
        return make_geometry_setup(ctx, std::move(geo), mesh_stats, atlas,
                                   static_cast<std::size_t>(np_stress));
    });

    // `STRING_SCENE` is now a LOOKUP, not a branch. An unknown name falls back to the default rather
    // than failing: a typo should cost you the scene you asked for, not the session.
    const std::string requested = cv_scene().get();
    const String::SceneRegistry::Scene* selected = registry.find(requested);
    if (selected == nullptr)
    {
        if (!requested.empty() && requested != "sponza")
            STRING_LOG_WARN("Unknown scene '{}' — falling back to 'sponza'", requested);
        selected = registry.find("sponza");
    }

    String::RenderPlan plan;
    plan.configure(selected->configure);
    registry.set_active(selected->name);
    return plan;
}

}  // namespace sandbox
