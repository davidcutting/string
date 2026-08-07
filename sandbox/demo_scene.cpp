#include "demo_scene.hpp"

#include <string/vulkan/passes/composite_pass.hpp>
#include <string/vulkan/frame_graph.hpp>
#include <string/render/geometry_pass.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <algorithm>
#include <chrono>

#include <string/ui/dynamic_font.hpp>
#include <string/ui/font.hpp>
#include <string/ui/layout.hpp>
#include <string/core/cvar.hpp>
#include <string/core/logger.hpp>
#include <string/vulkan/frame_graph.hpp>
#include <string/vulkan/content_root.hpp>
#include <string/vulkan/scene_registry.hpp>
#include "debug_cvars.hpp"
#include <string/render/debug_line_pass.hpp>
#include <string/render/geometry_pass.hpp>
#include <string/render/post_pass.hpp>
#include <string/render/ui_background_pass.hpp>
#include <string/render/shadertoy_pass.hpp>
#include <string/render/ui_pass.hpp>
#include "render_types.hpp"
#include <string/debug/panels.hpp>
#include <string/client/screens.hpp>
#include <string/render/render_debug.hpp>
#include <string/render/render_cvars.hpp>
#include <string/render/scene_loader.hpp>
#include <nlohmann/json.hpp>

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

// The app chooses these because it declares the attachments they describe. The renderer no longer
// has an opinion about sample count or swapchain format to supply.
constexpr VkSampleCountFlagBits kSceneSamples = VK_SAMPLE_COUNT_4_BIT;

// The INITIAL viewport, and only the initial one: everything viewport-derived is declared as a
// relationship, so a resize re-sizes backing without re-authoring anything.
VkExtent2D scene_viewport()
{
    VkExtent2D e{ 800, 800 };
    if (const char* ws = std::getenv("STRING_WINDOW_SIZE"))
    {
        unsigned w = 0, h = 0;
        if (std::sscanf(ws, "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64) e = { w, h };
    }
    return e;
}

// Brief 20: author_pass is DELETED. It existed to bridge a Pass object onto the graph by handing
// over its hand-written `usages` vector plus two record hooks — the second declaration channel and
// the two-entry-point pass, in one helper. A pass now declares itself; there is nothing to bridge.

// Defined below; the content-scan registry lambdas above call them.
struct scene_resources;
std::function<void(float)> make_geometry_scene(
    string::frame_graph& fg, String::engine_context& ctx, VkExtent2D viewport,
    VkSampleCountFlagBits samples, VkFormat swapchain_format,
    std::vector<std::filesystem::path> models,
    std::shared_ptr<string::dynamic_font_atlas> atlas, std::size_t np_stress, string::renderer& rr);
struct scene_resources;
std::function<void(float)> make_shader_scene(
    string::frame_graph& fg, String::engine_context& ctx, const std::filesystem::path& shader,
    std::shared_ptr<string::dynamic_font_atlas> atlas, string::renderer& rr);

// Throws naming the path rather than returning an empty vector. A silent empty read used to travel
// all the way into stb_truetype as a null pointer and fault there, which told a packager who forgot
// to ship assets/fonts/ nothing at all about what was missing.
std::vector<std::uint8_t> read_file(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot open '" + path.string() + "' (missing or unreadable)");

    std::vector<std::uint8_t> bytes{ std::istreambuf_iterator<char>(f),
                                     std::istreambuf_iterator<char>() };
    if (bytes.empty())
        throw std::runtime_error("'" + path.string() + "' is empty");
    return bytes;
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
ui_pass::DeferredAuthor make_deferred_author(SharedUi fluent)
{
    return [fluent] { return *fluent && (*fluent)->emit_next_deferred(); };
}

ui_pass::PostLayout make_ui_observer(SharedUi fluent,
                                    std::shared_ptr<string::ui::Workspace> ws = nullptr)
{
    return [fluent, ws](const string::layout_builder& b) {
        if (ws) ws->observe(b);
        if (*fluent) (*fluent)->observe();
    };
}

ui_pass::Author make_ui_author(std::shared_ptr<const MeshOverlayStats> mesh_stats,
                              SharedPanels panels,
                              std::size_t nameplate_stress, SharedUi fluent)
{
    return [mesh_stats,
            np_driver = UiSceneDriver(static_cast<std::uint32_t>(nameplate_stress)),
            np_scene = UiScene{}, motion = ui::Motion{}, ui_panels = string::ui::PanelStore{},
            fluent = std::move(fluent), nameplate_stress,
            panels, render_dbg = ::string::render::RenderDebug{}](
               string::layout_builder& b, const ui_pass::UiContext& ctx) mutable {
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
ui_pass::Author make_ui_dev_author(std::shared_ptr<UiScene> scene, std::uint32_t anchor_count,
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
               string::layout_builder& b, const ui_pass::UiContext& ctx) mutable {
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
// Brief 20 — the application declares its own resources and passes.
//
// Everything the scene renders into or through is stated HERE, once, as logical handles: the
// renderer creates no render targets, and no pass owns a viewport-sized image any more. Sizes that
// follow the window are declared as RELATIONSHIPS (viewport_scaled / bytes_for) rather than computed,
// which is what lets a resize be a backing swap instead of a re-author.
struct scene_resources
{
    string::gpu::image swapchain, color, depth, hdr;
    string::gpu::image hiz_depth, hiz_pyramid;
    string::gpu::image gtao_raw, gtao_ao;
    string::gpu::image bloom;
    string::gpu::image shadow[::string::render::kMaxCascades];
    string::gpu::image env_capture, env_prefiltered, dfg_lut;
    string::gpu::image hiz_depth_prev;
    string::gpu::image gi_irradiance, gi_cap_gbuf, gi_cap_albedo, gi_visibility;
    string::gpu::image gi_cube_albedo, gi_cube_nd, gi_cube_depth;
    string::gpu::buffer froxels, ibl_sh, worklists, transparency_list, histogram_bins;
    string::gpu::buffer scene_data, lights, stats;
    string::gpu::buffer gi_active, gi_offset, gi_meshlet_table;
    uint32_t hiz_mips = 0, bloom_mips = 0;
};

scene_resources declare_resources(string::frame_graph& fg, VkExtent2D viewport,
                                  VkSampleCountFlagBits samples, uint32_t shadow_res,
                                  uint32_t cascades, uint32_t max_draws,
                                  const ::string::render::ProbeVolume& gi_volume,
                                  std::size_t gi_table_entries)
{
    using namespace string;
    scene_resources r;

    // The one image whose backing the renderer still latches — it is not known until the frame
    // acquires it. Everything else the graph owns outright.
    r.swapchain = fg.use_persistent(string::persistent_image_info{ .name = "swapchain", .swapchain = true });

    // The multisampled scene targets and the single-sample HDR they resolve into. Declaring both as
    // colour writes on one pass is what tells the graph there is a resolve; there is no marker and no
    // hook.
    r.color = fg.image({ .name = "scene.color", .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                         .viewport_scaled = true, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                         .samples = samples });
    r.depth = fg.image({ .name = "scene.depth", .format = VK_FORMAT_D32_SFLOAT,
                         .viewport_scaled = true,
                         .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         .aspect = VK_IMAGE_ASPECT_DEPTH_BIT, .samples = samples });
    r.hdr = fg.image({ .name = "scene.hdr", .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                       .viewport_scaled = true,
                       .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                              | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                              | VK_IMAGE_USAGE_STORAGE_BIT });

    // HiZ. The depth ring is the one genuinely temporal image in the renderer — GTAO reprojection
    // reads the PREVIOUS frame slot's resolved depth — so it is per_frame, and gtao declares a second
    // handle over the same backing rotated one slot back.
    const VkExtent2D hz = ::string::render::geometry_pass::hiz_extent(viewport);
    r.hiz_mips = ::string::render::geometry_pass::hiz_mip_count(viewport);
    r.hiz_depth = fg.image({ .name = "hiz.depth", .format = VK_FORMAT_D32_SFLOAT,
                             .viewport_scaled = true,
                             .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             .aspect = VK_IMAGE_ASPECT_DEPTH_BIT, .per_frame = true });
    r.hiz_pyramid = fg.image({ .name = "hiz.pyramid", .format = VK_FORMAT_R32_SFLOAT,
                               .extent = { hz.width, hz.height, 1 }, .mip_levels = r.hiz_mips,
                               .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                               .sampler = { .mag_filter = VK_FILTER_NEAREST,
                                            .min_filter = VK_FILTER_NEAREST,
                                            .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                            .address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                            .anisotropy = false } });

    // GTAO, half res. `neutral` on the AO target is what a consumer gets when GTAO is toggled off:
    // a flat encoded bent normal with visibility 1, i.e. no occlusion — not black.
    const gpu::sampler_info clamp_linear{ .mag_filter = VK_FILTER_LINEAR, .min_filter = VK_FILTER_LINEAR,
                                          .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                          .address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                          .anisotropy = false };
    r.gtao_raw = fg.image({ .name = "gtao.raw", .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                            .viewport_scaled = true, .viewport_scale = 0.5f,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                            .sampler = clamp_linear });
    r.gtao_ao = fg.image({ .name = "gtao.ao", .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                           .viewport_scaled = true, .viewport_scale = 0.5f,
                           .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                           .sampler = clamp_linear,
                           .neutral = {{ 0.5f, 0.5f, 0.5f, 1.0f }} });

    // Shadow cascades: ONE image each, no ring. The three-deep ring was pure frames-in-flight
    // write-after-read avoidance, which the graph derives — 144 MB down to 48 MB. `neutral` 1.0 is
    // what makes toggling shadows off produce a lit unshadowed scene rather than a black one.
    for (uint32_t c = 0; c < cascades; ++c)
        r.shadow[c] = fg.image({
            .name = "shadow.cascade" + std::to_string(c), .format = VK_FORMAT_D32_SFLOAT,
            .extent = { shadow_res, shadow_res, 1 },
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
            .sampler = { .mag_filter = VK_FILTER_NEAREST, .min_filter = VK_FILTER_NEAREST,
                         .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                         .address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                         .anisotropy = false,
                         .border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE },
            .neutral = {{ 1.0f, 0.0f, 0.0f, 0.0f }} });

    // IBL: two 128^3 cubes with a 6-mip roughness ladder, the split-sum BRDF LUT, and the SH buffer.
    const gpu::sampler_info env_sampler{ .mag_filter = VK_FILTER_LINEAR, .min_filter = VK_FILTER_LINEAR,
                                         .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                         .address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                         .anisotropy = false,
                                         .border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE };
    r.env_capture = fg.image({ .name = "ibl.env_capture", .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                               .extent = { 128, 128, 1 }, .mip_levels = 6, .array_layers = 6, .cube = true,
                               .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               .sampler = env_sampler });
    r.env_prefiltered = fg.image({ .name = "ibl.env_prefiltered", .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                   .extent = { 128, 128, 1 }, .mip_levels = 6, .array_layers = 6, .cube = true,
                                   .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   .sampler = env_sampler });
    r.dfg_lut = fg.image({ .name = "ibl.dfg", .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                           .extent = { 128, 128, 1 },
                           .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                  | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           .sampler = env_sampler });

    // Bloom's mip count is fixed at author time, from the initial viewport — authored-once means the
    // number of declared passes cannot change on resize.
    r.bloom_mips = ::string::render::post_pass::bloom_mip_count(viewport);
    r.bloom = fg.image({ .name = "post.bloom", .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                         .viewport_scaled = true, .viewport_scale = 0.5f, .mip_levels = r.bloom_mips,
                         .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                         .sampler = clamp_linear });

    // --- buffers ---------------------------------------------------------------------------------
    r.froxels = fg.buffer({ .name = "froxels",
                            .bytes_for = [](VkExtent2D e) { return ::string::render::froxel_component::bytes_for(e); },
                            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT });
    r.ibl_sh = fg.buffer({ .name = "ibl.sh", .bytes = sizeof(float) * 4 * 9,
                           .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                  | VK_BUFFER_USAGE_TRANSFER_SRC_BIT });
    r.transparency_list = fg.buffer({
        .name = "transparency.list",
        .bytes = ::string::render::sorted_transparency::layout_for(max_draws).bytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
               | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
        .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .per_frame = true });
    // Probe GI. The atlases are tiled by the probe grid — the app sizes them from the SAME fit the
    // component uses (probe_gi_component::fit_volume), so neither side can drift from the other.
    const glm::uvec2 gi_tiles = gi_volume.tile_grid();
    const gpu::sampler_info gi_sampler{ .mag_filter = VK_FILTER_LINEAR, .min_filter = VK_FILTER_LINEAR,
                                        .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                        .address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .anisotropy = false };
    const VkImageUsageFlags gi_atlas_usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                           | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const auto gi_atlas = [&](const char* name, uint32_t stride) {
        return fg.image({ .name = name, .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .extent = { std::max(1u, gi_tiles.x * stride),
                                      std::max(1u, gi_tiles.y * stride), 1 },
                          .usage = gi_atlas_usage, .sampler = gi_sampler });
    };
    r.gi_irradiance  = gi_atlas("gi.irradiance",     ::string::render::kProbeIrradStride);
    r.gi_cap_gbuf    = gi_atlas("gi.capture_gbuf",   ::string::render::kProbeVisStride);
    r.gi_cap_albedo  = gi_atlas("gi.capture_albedo", ::string::render::kProbeVisStride);
    r.gi_visibility  = gi_atlas("gi.visibility",     ::string::render::kProbeVisStride);

    // The per-probe cube G-buffer, destructively reused between probes — pure write-after-read, no
    // history, so one set serves every probe.
    const auto gi_cube = [&](const char* name, VkFormat fmt, VkImageUsageFlags use, VkImageAspectFlags asp) {
        return fg.image({ .name = name, .format = fmt, .extent = { 32, 32, 1 },
                          .array_layers = 6, .cube = true, .usage = use, .aspect = asp,
                          .sampler = gi_sampler });
    };
    r.gi_cube_albedo = gi_cube("gi.cube_albedo", VK_FORMAT_R16G16B16A16_SFLOAT,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
    r.gi_cube_nd     = gi_cube("gi.cube_nd", VK_FORMAT_R16G16B16A16_SFLOAT,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
    r.gi_cube_depth  = gi_cube("gi.cube_depth", VK_FORMAT_D32_SFLOAT,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                               VK_IMAGE_ASPECT_DEPTH_BIT);

    const VkBufferUsageFlags gi_buf = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                    | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                    | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    r.gi_active = fg.buffer({ .name = "gi.active",
                              .bytes = std::max<VkDeviceSize>(4 * gi_volume.total(), 4),
                              .usage = gi_buf });
    r.gi_offset = fg.buffer({ .name = "gi.offset",
                              .bytes = std::max<VkDeviceSize>(16 * gi_volume.total(), 16),
                              .usage = gi_buf });
    r.gi_meshlet_table = fg.buffer({ .name = "gi.meshlet_table",
                                     .bytes = 8 * std::max<VkDeviceSize>(gi_table_entries, 1),
                                     .usage = gi_buf });

    r.histogram_bins = fg.buffer({ .name = "post.histogram.bins", .bytes = 256 * sizeof(uint32_t),
                                   .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                   .per_frame = true });

    // SceneData, the animated light ring and the GPU stats block. All three are host-written each
    // frame and read by that same frame's draws, so they ring for CPU-vs-GPU pacing.
    const VkBufferUsageFlags host_ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                       | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const VmaAllocationCreateFlags mapped = VMA_ALLOCATION_CREATE_MAPPED_BIT
                                          | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    r.scene_data = fg.buffer({ .name = "scene.data", .bytes = sizeof(::string::render::SceneData),
                               .usage = host_ssbo, .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
                               .allocation_flags = mapped, .per_frame = true });
    r.lights = fg.buffer({ .name = "scene.lights",
                           .bytes = sizeof(::string::render::GpuLight) * 1024u,
                           .usage = host_ssbo, .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
                           .allocation_flags = mapped, .per_frame = true });
    r.stats = fg.buffer({ .name = "scene.stats", .bytes = sizeof(::string::render::GpuMeshStats),
                          .usage = host_ssbo | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          .memory_usage = VMA_MEMORY_USAGE_GPU_TO_CPU,
                          .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT, .per_frame = true });

    // The worklist arena: draw-cull counts, scan offsets, and the compacted indirect command lists
    // for the camera, the two-sided set and every cascade. One buffer, many regions.
    // The meshlet worklist arena is the per-frame SCRATCH buffer — the passes reserve byte regions
    // from it at construction and address them as offsets, so the graph handle must name that same
    // allocation, not a separate one. Its backing is filled in after the arena materializes
    // (wire_scratch_arena), because reservations are only known once every pass has been built.
    r.worklists = fg.use_persistent(string::persistent_buffer_info{ .name = "meshlet.worklists" });

    // GTAO reads the PREVIOUS frame slot's resolved depth. Rather than inventing a "read at slot-1"
    // verb, that is a SECOND logical handle over the same physical ring, rotated one slot back. Both
    // handles resolve to the same VkImages, so the tracker sees one resource and derives the
    // cross-frame edge correctly — and it is persistent, because degrade must never substitute a
    // fallback for real history.
    r.hiz_depth_prev = fg.use_persistent(string::persistent_image_info{ .name = "hiz.depth_prev" });

    return r;
}


// A `.scene.json` descriptor: the manifest for a scene glTF alone cannot express — several files
// merged into one world, plus engine-side overrides. Deliberately small; anything glTF already says
// (cameras, lights, transforms, materials) is read from the ASSET, never restated here (brief 18).
//
//   { "name": "sponza",
//     "description": "Sponza — full scene",
//     "models": [ "sponza/main/X.gltf", "sponza/curtains/Y.gltf" ],
//     "camera": "35,30,25,-2.52,-0.51" }
//
// `models` paths resolve relative to the DESCRIPTOR'S OWN folder, so a content folder can be moved,
// copied or handed to someone else whole.
struct SceneDescriptor
{
    std::string name;
    std::string description;
    std::vector<std::filesystem::path> models;
    std::string camera;      // optional; empty = use the asset's own camera / derived framing
    // "geometry" (default) or "shader". Content-level, not a pipeline tuning: a Shadertoy scene is a
    // different KIND of thing, not a geometry scene with different settings (brief 18).
    std::string kind = "geometry";
    std::filesystem::path shader;   // kind == "shader"; empty = the built-in template
};

// Returns nullopt and logs on malformed input — one bad descriptor must not take the session down,
// the same posture as an unknown STRING_SCENE.
std::optional<SceneDescriptor> read_descriptor(const std::filesystem::path& file)
{
    std::ifstream in(file);
    if (!in)
    {
        STRING_LOG_WARN("[content] cannot read {}", file.string());
        return std::nullopt;
    }

    nlohmann::json j;
    try
    {
        in >> j;
    }
    catch (const std::exception& e)
    {
        STRING_LOG_WARN("[content] {} is not valid JSON: {}", file.string(), e.what());
        return std::nullopt;
    }

    SceneDescriptor d;
    // Default the name to the filename so `my_level.scene.json` needs only a `models` list.
    d.name = j.value("name", file.stem().stem().string());
    d.description = j.value("description", std::string{});
    d.camera = j.value("camera", std::string{});

    const std::filesystem::path base = file.parent_path();
    d.kind = j.value("kind", std::string{ "geometry" });
    for (const auto& m : j.value("models", std::vector<std::string>{}))
    {
        d.models.push_back(base / m);
    }

    if (d.kind == "shader")
    {
        if (const std::string s = j.value("shader", std::string{}); !s.empty())
        {
            d.shader = base / s;
        }
        return d;   // no models: a shader scene has no assets at all, which is the point
    }
    if (d.kind != "geometry")
    {
        STRING_LOG_WARN("[content] {}: unknown kind '{}' (expected \"geometry\" or \"shader\")",
                        file.string(), d.kind);
        return std::nullopt;
    }
    if (d.models.empty())
    {
        STRING_LOG_WARN("[content] {} lists no models — skipping", file.string());
        return std::nullopt;
    }
    return d;
}

// Scan the user's content root for glTF assets and register one scene per file.
//
// GeometryPass takes paths relative to resources_dir, but the content root is an arbitrary absolute
// folder the user chose — so pass the absolute path through and let the loader's `resources_path /
// rel` composition leave it alone (operator/ with an absolute right-hand side yields the absolute).
// That is why nothing here tries to relativise: an absolute content root simply works.
void register_content_scenes(String::SceneRegistry& registry,
                             const std::shared_ptr<string::dynamic_font_atlas>& atlas,
                             int np_stress)
{
    const std::filesystem::path& root = String::ContentRoot::get();
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec))
    {
        return;   // no content is a normal state, not an error
    }

    // Sorted for a stable menu order across runs (directory iteration order is not guaranteed).
    std::vector<std::filesystem::path> assets;
    std::vector<std::filesystem::path> descriptors;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec))
    {
        if (!entry.is_regular_file(ec))
        {
            continue;
        }
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".gltf" || ext == ".glb")
        {
            assets.push_back(entry.path());
        }
        else if (ext == ".json" && entry.path().stem().extension() == ".scene")
        {
            descriptors.push_back(entry.path());
        }
    }
    std::sort(assets.begin(), assets.end());
    std::sort(descriptors.begin(), descriptors.end());

    // Descriptors FIRST, so a multi-file scene claims its name before the bare-glTF pass runs and
    // the members of that scene do not also appear as standalone entries under the same name.
    std::set<std::filesystem::path> claimed;
    for (const std::filesystem::path& file : descriptors)
    {
        const std::optional<SceneDescriptor> d = read_descriptor(file);
        if (!d)
        {
            continue;
        }
        // A descriptor whose assets are absent is skipped, not fatal — this is what lets a
        // sponza.scene.json ship in the repo while the assets it names stay gitignored.
        std::error_code mec;
        const bool have_all = std::all_of(d->models.begin(), d->models.end(),
                                          [&](const std::filesystem::path& p) {
                                              return std::filesystem::exists(p, mec);
                                          });
        if (!have_all)
        {
            STRING_LOG_INFO("[content] '{}' skipped — its assets are not present", d->name);
            continue;
        }
        for (const std::filesystem::path& m : d->models)
        {
            claimed.insert(m);
        }
        if (!d->camera.empty())
        {
            // Same lever STRING_CAM feeds, so a descriptor camera and the env override are the one
            // mechanism. First descriptor with a camera wins; later ones do not stomp it.
            if (::string::render::cv_camera_pose().get().empty())
                ::string::render::cv_camera_pose().set(d->camera);
        }
        if (d->kind == "shader")
        {
            // Same pass shape as the `ui` scene — a fullscreen pass then the UI overlay, and no
            // PostProcessPass, which IS the post bypass. The error overlay rides on that UI pass,
            // so a shader that does not compile still has somewhere to report itself.
            const std::filesystem::path shader =
                d->shader.empty() ? std::filesystem::path{} : d->shader;
            registry.add(d->name,
                         d->description.empty() ? "Shader: " + file.filename().string()
                                                : d->description,
                         [atlas, shader](string::frame_graph& fg, String::engine_context& ctx,
                                         string::renderer& r) -> std::function<void(float)> {
                // Same shape as the `ui` scene: a fullscreen pass then the UI overlay, and NO post
                // chain — that absence IS the post bypass. The error overlay rides on the UI pass, so
                // a shader that fails to compile still has somewhere to report itself.
                const std::filesystem::path path =
                    shader.empty() ? ctx.resources_path / "shaders" / "shadertoy_default.slang"
                                   : shader;
                return make_shader_scene(fg, ctx, path, atlas, r);
            }, {}, /*from_content=*/true);
            continue;
        }

        registry.add(d->name, d->description.empty() ? "Content: " + file.filename().string()
                                                     : d->description,
                     [atlas, np_stress, models = d->models](string::frame_graph& fg,
                                                            String::engine_context& ctx,
                                                            string::renderer& r)
                       -> std::function<void(float)> {
            return make_geometry_scene(fg, ctx, scene_viewport(), kSceneSamples, r.swapchain_format(),
                                       models, atlas, static_cast<std::size_t>(np_stress), r);
        }, d->models, /*from_content=*/true);
    }

    // Drop assets a descriptor already composes, so sponza's three files do not also show up as
    // three separate one-file scenes.
    std::erase_if(assets, [&](const std::filesystem::path& p) { return claimed.contains(p); });

    for (const std::filesystem::path& asset : assets)
    {
        const std::string name = asset.stem().string();
        // A name collision would silently replace an earlier scene (SceneRegistry::add replaces on
        // duplicate), so qualify with the parent folder — two `scene.gltf` files in different dirs
        // are a completely normal way for an artist to organise work.
        const std::string unique = registry.find(name) == nullptr
                                 ? name
                                 : asset.parent_path().filename().string() + "/" + name;
        registry.add(unique, "Content: " + asset.filename().string(),
                     [atlas, np_stress, asset](string::frame_graph& fg, String::engine_context& ctx, string::renderer& r)
                       -> std::function<void(float)> {
            // NOTE: marked from_content below so a rescan can drop and re-add it.
            return make_geometry_scene(fg, ctx, scene_viewport(), kSceneSamples, r.swapchain_format(),
                                       std::vector<std::filesystem::path>{ asset }, atlas,
                                       static_cast<std::size_t>(np_stress), r);
        }, std::vector<std::filesystem::path>{ asset }, /*from_content=*/true);
    }
    STRING_LOG_INFO("[content] {} scene(s) discovered under {}", assets.size(), root.string());
}


// The geometry scene: construct every component, declare them onto the graph in execution order,
// and return the per-frame CPU tick.
//
// DECLARATION ORDER IS EXECUTION ORDER. The toposort breaks ties by authoring index, so this
// sequence is what the graph falls back on wherever two passes do not genuinely depend on each
// other. Two orderings here are LOAD-BEARING rather than cosmetic:
//   * ibl before gi     — GI relight reads this frame's sky SH (a read-after-write edge);
//   * gi  before shadow — GI relight SAMPLES the cascades the shadow passes then REWRITE, so the
//                         write-after-read edge only exists in that direction.
// Getting either wrong now yields a wrong derived barrier, which sync validation catches. The old
// push-order equivalent yielded a null device address on frame 0 and took the machine down.

struct geometry_scene_state
{
    std::shared_ptr<MeshOverlayStats> mesh_stats = std::make_shared<MeshOverlayStats>();
    std::unique_ptr<geometry_pass> geo;
    std::unique_ptr<froxel_component> froxel;
    std::unique_ptr<ibl_component> ibl;
    std::unique_ptr<gtao_chain> gtao;
    std::unique_ptr<probe_gi_component> gi;
    std::unique_ptr<sky_component> sky;
    std::unique_ptr<shadow_maps> shadow;
    std::unique_ptr<sorted_transparency> transparency;
    std::unique_ptr<debug_line_pass> debug_lines;
    std::unique_ptr<ui_pass> ui;
    std::unique_ptr<post_pass> post;
    std::unique_ptr<String::composite_pass> composite;
    scene_resources res;
    string::frame_graph* graph = nullptr;
    bool history_wired = false;
};

// GTAO reads the PREVIOUS frame slot's resolved depth. That is expressed as a SECOND logical handle
// over the SAME physical ring, rotated one slot back — no "read at slot-1" verb, no ping-pong, and
// no second allocation. It has to happen after compile(), because the ring's physicals are what the
// graph allocated; set_images() is exactly the backing-swap verb for it.
// Point the worklist handle at the materialized scratch arena, one physical per frame slot.
void wire_scratch_arena(geometry_scene_state& s, String::engine_context& ctx)
{
    if (!ctx.scratch.materialized()) return;
    std::vector<string::gpu::resource_id> slots;
    for (std::uint32_t i = 0; i < ctx.frames_in_flight; ++i) slots.push_back(ctx.scratch.buffer(i));
    s.graph->set_buffers(s.res.worklists, slots);
}

void wire_depth_history(geometry_scene_state& s)
{
    if (s.history_wired || s.graph == nullptr) return;
    const std::vector<string::gpu::resource_id>& ring = s.graph->record_of(s.res.hiz_depth).physical;
    if (ring.empty()) return;
    std::vector<string::gpu::resource_id> rotated(ring.size());
    for (std::size_t i = 0; i < ring.size(); ++i)
        rotated[i] = ring[(i + ring.size() - 1) % ring.size()];
    s.graph->set_images(s.res.hiz_depth_prev, rotated);
    s.history_wired = true;
}

// The shadertoy scene: one fullscreen pass, the UI overlay, and the composite. Deliberately NO post
// chain — that absence IS the post bypass.
struct shader_scene_state
{
    std::unique_ptr<shadertoy_pass> toy;
    std::unique_ptr<ui_pass> ui;
    std::unique_ptr<String::composite_pass> composite;
    scene_resources res;
};

std::function<void(float)> make_shader_scene(
    string::frame_graph& fg, String::engine_context& ctx, const std::filesystem::path& shader,
    std::shared_ptr<string::dynamic_font_atlas> atlas, string::renderer& rr)
{
    auto s = std::make_shared<shader_scene_state>();
    s->res = declare_resources(fg, scene_viewport(), kSceneSamples, 2048, 0, 0,
                               ::string::render::ProbeVolume{}, 0);
    s->toy = std::make_unique<shadertoy_pass>(ctx, shader, kSceneSamples);
    auto toy_ui_slot = std::make_shared<std::optional<ui::Ui>>();
    auto toy_panels = std::make_shared<debug::DebugPanels>();
    s->ui = std::make_unique<ui_pass>(
        ctx, kSceneSamples, atlas,
        make_ui_author(std::make_shared<MeshOverlayStats>(), toy_panels, 0, toy_ui_slot),
        make_ui_observer(toy_ui_slot), make_deferred_author(toy_ui_slot));
    s->composite = std::make_unique<String::composite_pass>(ctx, rr.swapchain_format());

    s->toy->declare(fg, s->res.color, s->res.depth);
    s->ui->declare(fg, s->res.color);
    s->composite->declare(fg, s->res.hdr, s->res.swapchain);
    rr.capture_source(s->res.hdr);

    return [s](float dt) {
        s->toy->tick(dt);
        s->ui->tick(dt, scene_viewport(), 0);
        s->composite->tick();
    };
}

std::function<void(float)> make_geometry_scene(
    string::frame_graph& fg, String::engine_context& ctx, VkExtent2D viewport,
    VkSampleCountFlagBits samples, VkFormat swapchain_format,
    std::vector<std::filesystem::path> models,
    std::shared_ptr<string::dynamic_font_atlas> atlas, std::size_t np_stress, string::renderer& rr)
{
    auto s = std::make_shared<geometry_scene_state>();
    s->graph = &fg;

    s->geo = std::make_unique<geometry_pass>(ctx, samples, std::move(models), s->mesh_stats);
    ::string::render::GeometryScene* scene = s->geo->scene();

    const ::string::render::ProbeVolume gi_volume =
        probe_gi_component::fit_volume(scene->scene_aabb_min_, scene->scene_aabb_max_);
    s->res = declare_resources(fg, viewport, samples, scene->settings_.shadow_resolution,
                               scene->settings_.cascade_count, scene->cull_max_draws_,
                               gi_volume, s->geo->gi_capture_entries());
    const scene_resources& r = s->res;

    s->froxel       = std::make_unique<froxel_component>(ctx);
    s->ibl          = std::make_unique<ibl_component>(ctx);
    s->gtao         = std::make_unique<gtao_chain>(ctx, scene);
    s->gi           = std::make_unique<probe_gi_component>(ctx, scene, samples);
    s->sky          = std::make_unique<sky_component>(ctx, samples);
    s->shadow       = std::make_unique<shadow_maps>(ctx, scene);
    s->transparency = std::make_unique<sorted_transparency>(ctx, scene, samples);
    s->debug_lines  = std::make_unique<debug_line_pass>(ctx, s->mesh_stats, samples);
    auto ui_slot = std::make_shared<std::optional<ui::Ui>>();
    auto dbg_panels = std::make_shared<debug::DebugPanels>();
    s->ui = std::make_unique<ui_pass>(
        ctx, samples, atlas,
        make_ui_author(s->mesh_stats, dbg_panels, np_stress, ui_slot),
        make_ui_observer(ui_slot), make_deferred_author(ui_slot));
    s->post         = std::make_unique<post_pass>(ctx);
    s->composite    = std::make_unique<String::composite_pass>(ctx, swapchain_format);

    const std::span<const string::gpu::image> cascades{ r.shadow, scene->settings_.cascade_count };
    const probe_gi_component::resources gi_res{
        r.gi_irradiance, r.gi_cap_gbuf, r.gi_cap_albedo, r.gi_visibility,
        r.gi_cube_albedo, r.gi_cube_nd, r.gi_cube_depth,
        r.gi_active, r.gi_offset, r.gi_meshlet_table };

    s->froxel->declare(fg, r.froxels, r.lights);
    s->ibl->declare(fg, r.env_capture, r.env_prefiltered, r.dfg_lut, r.ibl_sh);
    s->gtao->declare(fg, r.hiz_depth_prev, r.gtao_raw, r.gtao_ao);
    s->gi->declare(fg, gi_res, r.ibl_sh, r.scene_data, cascades, r.color, r.depth);
    s->shadow->declare(fg, cascades, r.worklists);
    s->sky->declare(fg, r.color, r.depth, [] { return ::string::render::cv_pass_sky().get(); });
    s->geo->declare(fg, r.color, r.hdr, r.depth, r.hiz_depth, r.hiz_pyramid, r.hiz_mips,
                    r.worklists, r.scene_data, r.lights, r.stats,
                    cascades, r.gtao_ao, r.env_prefiltered, r.dfg_lut, r.ibl_sh, r.froxels);
    s->transparency->declare(fg, r.color, r.depth, r.transparency_list, r.scene_data, r.stats);
    s->debug_lines->declare(fg, r.color, r.depth);
    s->ui->declare(fg, r.color);
    s->post->declare(fg, r.hdr, r.bloom, r.bloom_mips, r.histogram_bins);
    s->composite->declare(fg, r.hdr, r.swapchain);

    // The headless gates capture the resolved HDR target — an app-declared transient, so the app is
    // what names it. This is the seam the renderer cannot fill on its own.
    rr.capture_source(r.hdr);

    // The per-frame tick. Ordinary app code: nothing here resolves a device address or a bindless
    // slot — every one of those is looked up through pass_context while its owning pass records.
    ::string::render::GeometryScene* scene_ptr = scene;
    String::engine_context* ctx_ptr = &ctx;
    string::renderer* rp = &rr;
    return [s, scene_ptr, ctx_ptr, rp](float dt) {
        // The live viewport. This used to arrive via Pass::resize(); with that gone, the scene state
        // carries it and the app is what knows it. Zero here means every viewport-derived layout —
        // the UI most visibly — computes against a 0x0 screen and draws nothing.
        scene_ptr->screen_size = rp->extent();
        wire_depth_history(*s);
        wire_scratch_arena(*s, *ctx_ptr);
        const std::uint32_t slot = rp->frame_slot();
        s->geo->tick(dt, slot);
        s->gtao->tick(rp->extent(), static_cast<uint16_t>(slot));
        s->ibl->tick(scene_ptr->ibl_lighting(), false);
        s->gi->tick();
        s->shadow->tick();
        s->froxel->tick(scene_ptr->froxel_params());
        s->sky->tick(scene_ptr->sky_params());
        s->ui->tick(dt, rp->extent(), slot);
        s->post->tick(dt);
        s->composite->tick();
    };
}
}  // namespace

String::Application::scene_fn build_demo_scene(const std::filesystem::path& resources_dir)
{
    // The UI's SDF atlas is baked once, up front and shared: the UI pass's per-frame layout measures
    // against it and draws text from it.
    const std::vector<std::uint8_t> ttf = read_file(resources_dir / "assets/fonts/DejaVuSans.ttf");
    auto atlas = std::make_shared<string::dynamic_font_atlas>(ttf);

    // Content defaults to the resources dir's assets/. Must precede any ContentRoot::get(), which
    // memoises on first call.
    String::ContentRoot::set_default(resources_dir / "assets");

    std::vector<std::filesystem::path> models;
    if (const char* scene = std::getenv("STRING_SCENE"))
    {
        const std::filesystem::path p = String::ContentRoot::get() / scene;
        if (std::filesystem::exists(p)) models.push_back(p);
    }

    const std::size_t np_stress = static_cast<std::size_t>(std::max(0, cv_ui_nameplates().get()));

    // Populate the scene registry so the Scene panel can LIST what is on disk. Registration was
    // dropped when build_demo_plan became build_demo_scene, which is why the dropdown was empty.
    // NOTE: listing is all this restores — SELECTING a scene still does nothing, because switching
    // needs the app to tear down its pass objects and author a fresh graph (see the TODO in
    // application.cpp). A menu that lists scenes and silently ignores clicks is its own trap, so
    // that gap is called out here rather than left to be discovered.
    register_content_scenes(String::SceneRegistry::instance(), atlas, static_cast<int>(np_stress));

    // Brief 20: the app hands back ONE callback. It constructs the scene against the ready graph and
    // engine context, declares every pass, and returns the per-frame tick. There is no pass list
    // alongside it — that second list, and the unenforced agreement between the two, is what this
    // brief deleted.
    return [atlas, models, np_stress](string::frame_graph& fg, String::engine_context& ctx, string::renderer& r)
             -> std::function<void(float)> {
        return make_geometry_scene(fg, ctx, scene_viewport(), kSceneSamples, r.swapchain_format(),
                                   models, atlas, np_stress, r);
    };
}

}  // namespace sandbox
