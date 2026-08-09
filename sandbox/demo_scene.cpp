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
    string::frame_graph& fg, string::engine_context& ctx, VkExtent2D viewport,
    VkSampleCountFlagBits samples, VkFormat swapchain_format,
    std::vector<std::filesystem::path> models,
    std::shared_ptr<string::dynamic_font_atlas> atlas, std::size_t np_stress, string::renderer& rr,
    bool lookdev = false);
struct scene_resources;
// Per-frame streaming uploads, INSIDE the frame (brief 21 step 5). Authored first so the copies land
// before anything samples this frame; they touch resources the graph does not own — streamed
// textures reached through bindless slots, the geometry heaps — so there is nothing for it to
// declare, and the pass states that undeclarable edge itself with one conservative barrier (see
// TransferBatch::record). It replaced a second submission path that ran beside the frame.
//
// EVERY SCENE MUST DECLARE THIS, which is why it is a shared helper rather than a block inside one
// scene. It is engine plumbing, not geometry plumbing: TransferBatch is an engine service that
// stages uploads for anyone, and this pass is the ONLY thing that records them. It used to live only
// in make_geometry_scene, so the UI scene staged composite_pass's tonemap LUT and nothing ever
// copied it — the composite sampled an empty LUT and presented BLACK. Captures hid it completely,
// because declare_capture tonemaps on the CPU through lut_cpu_ and never touches the GPU LUT.
void declare_stream_uploads(string::frame_graph& fg, string::engine_context& ctx);
std::function<void(float)> make_shader_scene(
    string::frame_graph& fg, string::engine_context& ctx, const std::filesystem::path& shader,
    std::shared_ptr<string::dynamic_font_atlas> atlas, string::renderer& rr);
std::function<void(float)> make_ui_scene(
    string::frame_graph& fg, string::engine_context& ctx,
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
// follow the window are declared as RELATIONSHIPS (viewport_fit / tile_pixels) rather than computed,
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
    string::gpu::buffer froxels, ibl_sh, transparency_list, histogram_bins;
    ::string::render::WorklistSet worklists;
    string::gpu::buffer scene_data, lights, stats;
    string::gpu::buffer gi_active, gi_offset, gi_meshlet_table;
    uint32_t bloom_mips = 0;
};

// Brief 21 D3 — time lives OUTSIDE the graph. Every resource whose CONTENTS outlive the frame
// (cross-frame history, host-write pacing rings, in-place accumulators) has a lifetime beyond the
// graph's scope, so the APP creates and destroys its backing and hands it in as a persistent; the
// graph tracks usage only. This struct is that owner for the geometry scene. The category test:
// contents outlive the frame -> persistent (here); frame-scoped -> transient (fg.image/fg.buffer).
struct scene_backing
{
    string::engine_context* ctx = nullptr;
    // The depth-history ring: GTAO reprojection reads LAST frame's resolved depth, so the app backs
    // one image per frame in flight and declares TWO handles over them — identity order for this
    // frame's write, rotated one slot back for last frame's read. The rotation is static: resolved
    // by frame slot, no per-frame swap call, no reach into graph records.
    std::vector<string::gpu::resource_id> hiz_depth;
    // Host-write pacing rings: the CPU fills frame N+1's slot while the GPU still reads frame N's.
    std::vector<string::gpu::resource_id> scene_data, lights, stats, transparency_list;
    // Probe-GI accumulators: captured/relit tiles persist across the amortized rounds, and readers
    // must see REAL accumulated data on frames when no producer pass runs — as transients they were
    // degrade-substituted with the 1x1 neutral on exactly those frames.
    string::gpu::resource_id gi_irradiance = 0, gi_cap_gbuf = 0, gi_cap_albedo = 0, gi_visibility = 0;
    string::gpu::resource_id gi_active = 0, gi_offset = 0, gi_meshlet_table = 0;
    // IBL products: the bake is AMORTIZED — it re-runs only when the sun moves, so on nearly every
    // frame nothing writes them and everything reads them. Same category as the GI atlases, and the
    // same failure when they were transients: a resize destroyed their backing, no bake re-ran, and
    // the whole scene shaded against 1x1 neutral env/DFG until the sun happened to move.
    string::gpu::resource_id env_capture = 0, env_prefiltered = 0, dfg_lut = 0, ibl_sh = 0;

    void create(string::engine_context& c, VkExtent2D viewport, uint32_t max_draws,
                const ::string::render::ProbeVolume& gi_volume, std::size_t gi_table_entries)
    {
        ctx = &c;
        const uint32_t slots = c.frames_in_flight;

        create_hiz_depth(viewport);

        const VkBufferUsageFlags host_ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                           | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        const VmaAllocationCreateFlags mapped = VMA_ALLOCATION_CREATE_MAPPED_BIT
                                              | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        const auto ring = [&](std::vector<string::gpu::resource_id>& out, string::gpu::buffer_info info) {
            for (uint32_t s = 0; s < slots; ++s) out.push_back(c.allocator.create_resource(info));
        };
        ring(scene_data, { .size = sizeof(::string::render::SceneData), .usage = host_ssbo,
                           .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU, .allocation_flags = mapped });
        ring(lights, { .size = sizeof(::string::render::GpuLight) * 1024u, .usage = host_ssbo,
                       .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU, .allocation_flags = mapped });
        ring(stats, { .size = sizeof(::string::render::GpuMeshStats),
                      .usage = host_ssbo | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      .memory_usage = VMA_MEMORY_USAGE_GPU_TO_CPU,
                      .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT });
        ring(transparency_list, { .size = ::string::render::sorted_transparency::layout_for(max_draws).bytes,
                                  .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                         | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                         | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                  .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
                                  .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                                                    | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT });

        const glm::uvec2 gi_tiles = gi_volume.tile_grid();
        const string::gpu::sampler_info gi_sampler{ .mag_filter = VK_FILTER_LINEAR,
                                                    .min_filter = VK_FILTER_LINEAR,
                                                    .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                                    .address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                    .anisotropy = false };
        const auto gi_atlas = [&](uint32_t stride) {
            string::gpu::image_info info{};
            info.extent = { std::max(1u, gi_tiles.x * stride), std::max(1u, gi_tiles.y * stride), 1 };
            info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            info.aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT;
            info.memory_usage = VMA_MEMORY_USAGE_GPU_ONLY;
            info.sampler = gi_sampler;
            return c.allocator.create_resource(info);
        };
        gi_irradiance = gi_atlas(::string::render::kProbeIrradStride);
        gi_cap_gbuf   = gi_atlas(::string::render::kProbeVisStride);
        gi_cap_albedo = gi_atlas(::string::render::kProbeVisStride);
        gi_visibility = gi_atlas(::string::render::kProbeVisStride);

        const VkBufferUsageFlags gi_buf = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                        | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        gi_active = c.allocator.create_resource(string::gpu::buffer_info{
            .size = std::max<VkDeviceSize>(4 * gi_volume.total(), 4), .usage = gi_buf,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY, .allocation_flags = 0 });
        gi_offset = c.allocator.create_resource(string::gpu::buffer_info{
            .size = std::max<VkDeviceSize>(16 * gi_volume.total(), 16), .usage = gi_buf,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY, .allocation_flags = 0 });
        gi_meshlet_table = c.allocator.create_resource(string::gpu::buffer_info{
            .size = 8 * std::max<VkDeviceSize>(gi_table_entries, 1), .usage = gi_buf,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY, .allocation_flags = 0 });

        // IBL: two 128^3 cubes with a 6-mip roughness ladder, the split-sum BRDF LUT, and the SH
        // buffer. Viewport-INDEPENDENT, so a resize leaves them alone — which is the point: the bake
        // that fills them is amortized and will not re-run just because the window changed.
        const string::gpu::sampler_info env_sampler{
            .mag_filter = VK_FILTER_LINEAR, .min_filter = VK_FILTER_LINEAR,
            .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .anisotropy = false,
            .border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE };
        const auto env_cube = [&] {
            string::gpu::image_info info{};
            info.extent = { 128, 128, 1 };
            info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            info.aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT;
            info.memory_usage = VMA_MEMORY_USAGE_GPU_ONLY;
            info.sampler = env_sampler;
            info.mip_levels = 6;
            info.cube = true;   // 6 array layers + CUBE_COMPATIBLE, per image_info::cube
            return c.allocator.create_resource(info);
        };
        env_capture = env_cube();
        env_prefiltered = env_cube();
        {
            string::gpu::image_info info{};
            info.extent = { 128, 128, 1 };
            info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            info.aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT;
            info.memory_usage = VMA_MEMORY_USAGE_GPU_ONLY;
            info.sampler = env_sampler;
            dfg_lut = c.allocator.create_resource(info);
        }
        ibl_sh = c.allocator.create_resource(string::gpu::buffer_info{
            .size = sizeof(float) * 4 * 9,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                   | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY, .allocation_flags = 0 });
    }

    // The depth-history ring is viewport-sized, so ITS OWNER re-creates it on resize (the graph
    // only re-backs what it owns — the viewport-scaled transients). Called at creation and from the
    // renderer's resize callback, under device idle.
    void create_hiz_depth(VkExtent2D viewport)
    {
        for (string::gpu::resource_id id : hiz_depth) ctx->allocator.destroy_resource(id);
        hiz_depth.clear();
        string::gpu::image_info depth_info{};
        depth_info.extent = { viewport.width, viewport.height, 1 };
        depth_info.format = VK_FORMAT_D32_SFLOAT;
        depth_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        depth_info.aspect_flags = VK_IMAGE_ASPECT_DEPTH_BIT;
        depth_info.memory_usage = VMA_MEMORY_USAGE_GPU_ONLY;
        for (uint32_t s = 0; s < ctx->frames_in_flight; ++s)
            hiz_depth.push_back(ctx->allocator.create_resource(depth_info));
    }

    ~scene_backing()
    {
        if (ctx == nullptr) return;
        for (string::gpu::resource_id id : hiz_depth) ctx->allocator.destroy_resource(id);
        for (auto* v : { &scene_data, &lights, &stats, &transparency_list })
            for (string::gpu::resource_id id : *v) ctx->allocator.destroy_resource(id);
        for (string::gpu::resource_id id : { gi_irradiance, gi_cap_gbuf, gi_cap_albedo, gi_visibility,
                                             gi_active, gi_offset, gi_meshlet_table,
                                             env_capture, env_prefiltered, dfg_lut, ibl_sh })
            if (id != 0) ctx->allocator.destroy_resource(id);
    }
};

scene_resources declare_resources(string::frame_graph& fg, VkExtent2D viewport,
                                  VkSampleCountFlagBits samples, uint32_t shadow_res,
                                  uint32_t cascades, VkDeviceSize worklist_bytes,
                                  VkDeviceSize draw_lod_bytes, const scene_backing* backing)
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
                         .fit = string::viewport_fit::scaled, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                         .samples = samples });
    r.depth = fg.image({ .name = "scene.depth", .format = VK_FORMAT_D32_SFLOAT,
                         .fit = string::viewport_fit::scaled,
                         .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         .aspect = VK_IMAGE_ASPECT_DEPTH_BIT, .samples = samples });
    r.hdr = fg.image({ .name = "scene.hdr", .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                       .fit = string::viewport_fit::scaled,
                       .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                              | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                              | VK_IMAGE_USAGE_STORAGE_BIT });

    // HiZ. The depth history is the one genuinely temporal image in the renderer — GTAO reprojection
    // reads the PREVIOUS frame slot's resolved depth. Its contents outlive the frame, so per brief
    // 21 D3 the APP backs it (scene_backing, one image per frame in flight) and declares TWO
    // persistent handles over the same physicals: identity order for this frame's write, rotated
    // one slot back for last frame's read. The rotation is STATIC — declared once, resolved by
    // frame slot, no per-frame wiring and no reach into graph records.
    {
        std::vector<gpu::resource_id> ring, rotated;
        if (backing != nullptr)
        {
            ring = backing->hiz_depth;
            rotated.resize(ring.size());
            for (std::size_t i = 0; i < ring.size(); ++i)
                rotated[i] = ring[(i + ring.size() - 1) % ring.size()];
        }
        r.hiz_depth = fg.use_persistent(string::persistent_image_info{
            .name = "hiz.depth", .physical = ring });
        r.hiz_depth_prev = fg.use_persistent(string::persistent_image_info{
            .name = "hiz.depth_prev", .physical = rotated });
    }
    // The occlusion pyramid FOLLOWS THE WINDOW: half the next power of two, full chain. Both halves
    // of that are declared relationships, not numbers, so a resize re-backs it without re-authoring —
    // and the reduction chain that builds it is authored once at the maximum depth with the levels
    // this extent does not reach conditioned off (geometry_pass::declare_hiz).
    r.hiz_pyramid = fg.image({ .name = "hiz.pyramid", .format = VK_FORMAT_R32_SFLOAT,
                               .fit = string::viewport_fit::half_pow2,
                               .mip_levels = string::transient_image_info::all_mips,
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
                            .fit = string::viewport_fit::scaled, .viewport_scale = 0.5f,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                            .sampler = clamp_linear });
    r.gtao_ao = fg.image({ .name = "gtao.ao", .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                           .fit = string::viewport_fit::scaled, .viewport_scale = 0.5f,
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
            // TRANSFER_SRC so STRING_CAPTURE_SOURCE=shadowN can actually read one. Without it the
            // copy is invalid usage, which is why that lever never produced a real depth dump.
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
            .sampler = { .mag_filter = VK_FILTER_NEAREST, .min_filter = VK_FILTER_NEAREST,
                         .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                         .address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                         .anisotropy = false,
                         .border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE },
            // REVERSE-Z: 0.0 is the far plane, so 0.0 means "no occluder anywhere" and reads as LIT.
            // It was declared 1.0 — the near plane — which is an occluder in front of everything, so
            // degrading to it shadowed the entire scene. The old `cascade_count = 0` ternary hid that
            // by making the shader skip the lookup entirely; deleting the ternary (step 4) is what
            // exposed it, which is exactly what the per-toggle gate is for.
            .neutral = {{ 0.0f, 0.0f, 0.0f, 0.0f }} });

    // Bloom's mip count is fixed at author time, from the initial viewport — authored-once means the
    // number of declared passes cannot change on resize.
    r.bloom_mips = ::string::render::post_pass::bloom_mip_count(viewport);
    r.bloom = fg.image({ .name = "post.bloom", .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                         .fit = string::viewport_fit::scaled, .viewport_scale = 0.5f, .mip_levels = r.bloom_mips,
                         .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                         .sampler = clamp_linear });

    // --- buffers ---------------------------------------------------------------------------------
    r.froxels = fg.buffer({ .name = "froxels",
                            // One [count, light indices...] record per froxel, and a froxel column
                            // per screen tile — the relationship, stated rather than computed.
                            .tile_pixels = ::string::render::kFroxelTileSize,
                            .bytes_per_tile = ::string::render::froxel_component::bytes_per_tile(),
                            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT });
    // Host-write pacing rings (brief 21 D3): the CPU fills frame N+1's slot while the GPU reads
    // frame N's, so their contents outlive the frame and the APP backs them, one slot per frame in
    // flight. The graph resolves by slot exactly as it did when these were per_frame transients.
    const auto persist_buf = [&](const char* name, const std::vector<gpu::resource_id>* phys) {
        return fg.use_persistent(string::persistent_buffer_info{
            .name = name, .physical = phys != nullptr ? *phys : std::vector<gpu::resource_id>{} });
    };
    r.transparency_list = persist_buf("transparency.list",
                                      backing != nullptr ? &backing->transparency_list : nullptr);
    // Probe GI accumulators (brief 21 D3): captured and relit tiles PERSIST across the amortized
    // rounds — most frames run neither capture nor relight, and readers must see the real
    // accumulated data on exactly those frames. As transients they were degrade-substituted with
    // the 1x1 neutral whenever no producer pass survived, which blanked a bake that was perfectly
    // good. Contents outlive the frame -> the app backs them (scene_backing), sized from the SAME
    // fit the component uses (probe_gi_component::fit_volume), so neither side can drift.
    const auto persist_img = [&](const char* name, gpu::resource_id id) {
        return fg.use_persistent(string::persistent_image_info{
            .name = name,
            .physical = id != 0 ? std::vector<gpu::resource_id>{ id } : std::vector<gpu::resource_id>{} });
    };
    // IBL products, same category and the same reason (see scene_backing): amortized producers whose
    // contents must survive both an idle frame and a resize.
    r.env_capture     = persist_img("ibl.env_capture",     backing != nullptr ? backing->env_capture : 0);
    r.env_prefiltered = persist_img("ibl.env_prefiltered", backing != nullptr ? backing->env_prefiltered : 0);
    r.dfg_lut         = persist_img("ibl.dfg",             backing != nullptr ? backing->dfg_lut : 0);

    r.gi_irradiance = persist_img("gi.irradiance",     backing != nullptr ? backing->gi_irradiance : 0);
    r.gi_cap_gbuf   = persist_img("gi.capture_gbuf",   backing != nullptr ? backing->gi_cap_gbuf : 0);
    r.gi_cap_albedo = persist_img("gi.capture_albedo", backing != nullptr ? backing->gi_cap_albedo : 0);
    r.gi_visibility = persist_img("gi.visibility",     backing != nullptr ? backing->gi_visibility : 0);

    // The per-probe cube G-buffer, destructively reused between probes — pure write-after-read, no
    // history, so one set serves every probe. Genuinely frame-scoped -> stays a graph transient.
    const gpu::sampler_info gi_sampler{ .mag_filter = VK_FILTER_LINEAR, .min_filter = VK_FILTER_LINEAR,
                                        .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                        .address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .anisotropy = false };
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

    // GI table/state buffers: uploaded at bake start, read for the volume's whole lifetime —
    // contents outlive the frame, app-backed like the atlases.
    const auto persist_one_buf = [&](const char* name, gpu::resource_id id) {
        return fg.use_persistent(string::persistent_buffer_info{
            .name = name,
            .physical = id != 0 ? std::vector<gpu::resource_id>{ id } : std::vector<gpu::resource_id>{} });
    };
    r.gi_active        = persist_one_buf("gi.active",        backing != nullptr ? backing->gi_active : 0);
    r.gi_offset        = persist_one_buf("gi.offset",        backing != nullptr ? backing->gi_offset : 0);
    r.gi_meshlet_table = persist_one_buf("gi.meshlet_table", backing != nullptr ? backing->gi_meshlet_table : 0);
    r.ibl_sh           = persist_one_buf("ibl.sh",           backing != nullptr ? backing->ibl_sh : 0);

    // Histogram bins are written and consumed within one frame — a true transient; the cross-frame
    // write-after-read against last frame's resolve derives from tracked state.
    r.histogram_bins = fg.buffer({ .name = "post.histogram.bins", .bytes = 256 * sizeof(uint32_t),
                                   .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT });

    // SceneData, the animated light ring and the GPU stats block — host-write pacing rings,
    // app-backed (see scene_backing).
    r.scene_data = persist_buf("scene.data",   backing != nullptr ? &backing->scene_data : nullptr);
    r.lights     = persist_buf("scene.lights", backing != nullptr ? &backing->lights : nullptr);
    r.stats      = persist_buf("scene.stats",  backing != nullptr ? &backing->stats : nullptr);

    // The GPU work lists (brief 21 D4): ONE TRANSIENT PER LIST — the camera's opaque and two-sided
    // lists, one per possible cascade, and the shared per-draw LOD. They were regions of a per-frame
    // scratch arena the app had to wire in after the fact; as declarations the graph owns their
    // lifetime, their aliasing and the edges between them, and the cascade chains stop serialising
    // behind the camera's just because they shared an allocation.
    const VkBufferUsageFlags wl_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                      | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                      | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                      | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const auto list = [&](const std::string& name, VkDeviceSize bytes) {
        return fg.buffer({ .name = name, .bytes = bytes, .usage = wl_usage });
    };
    r.worklists.opaque   = list("meshlet.worklist.opaque", worklist_bytes);
    r.worklists.twosided = list("meshlet.worklist.twosided", worklist_bytes);
    for (uint32_t c = 0; c < ::string::render::kMaxCascades; ++c)
        r.worklists.cascade[c] = list("meshlet.worklist.cascade" + std::to_string(c), worklist_bytes);
    r.worklists.draw_lod = list("meshlet.draw_lod", draw_lod_bytes);

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
void register_content_scenes(string::SceneRegistry& registry,
                             const std::shared_ptr<string::dynamic_font_atlas>& atlas,
                             int np_stress)
{
    const std::filesystem::path& root = string::ContentRoot::get();
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
                         [atlas, shader](string::frame_graph& fg, string::engine_context& ctx,
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
                                                            string::engine_context& ctx,
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
                     [atlas, np_stress, asset](string::frame_graph& fg, string::engine_context& ctx, string::renderer& r)
                       -> std::function<void(float)> {
            // NOTE: marked from_content below so a rescan can drop and re-add it.
            return make_geometry_scene(fg, ctx, scene_viewport(), kSceneSamples, r.swapchain_format(),
                                       std::vector<std::filesystem::path>{ asset }, atlas,
                                       static_cast<std::size_t>(np_stress), r);
        }, std::vector<std::filesystem::path>{ asset }, /*from_content=*/true);
    }
    // Report BOTH counts. This used to print only `assets`, which is the leftovers after erase_if
    // dropped everything a descriptor claimed — so a content root holding nothing but sponza's three
    // glTFs and its descriptor logged "0 scene(s) discovered" while having registered sponza fine.
    STRING_LOG_INFO("[content] {} descriptor scene(s) + {} bare asset(s) under {}",
                    descriptors.size(), assets.size(), root.string());
    // The NAMES, not just the counts. A switch is addressed by name (the F1 menu, STRING_SCENE,
    // STRING_SCENE_SWITCH), and SceneRegistry::request() silently no-ops on a name it does not
    // know — so "nothing happened when I clicked" and "that asset is not registered" look identical
    // from outside. Printing the registry once removes the guesswork.
    std::string names;
    for (const string::SceneRegistry::Scene& s : registry.scenes())
    {
        if (!names.empty()) names += ", ";
        names += s.name;
    }
    STRING_LOG_INFO("[content] switchable scenes: {}", names);
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
    // Declared FIRST so it is destroyed LAST: passes may record against these backings up to the
    // wait_idle that precedes scene teardown, and the ids must outlive every pass that named them.
    scene_backing backing;
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
    std::unique_ptr<string::composite_pass> composite;
    scene_resources res;
    string::frame_graph* graph = nullptr;

};

// The shadertoy scene: one fullscreen pass, the UI overlay, and the composite. Deliberately NO post
// chain — that absence IS the post bypass.
struct shader_scene_state
{
    std::unique_ptr<shadertoy_pass> toy;
    std::unique_ptr<ui_pass> ui;
    std::unique_ptr<string::composite_pass> composite;
    scene_resources res;
};

std::function<void(float)> make_shader_scene(
    string::frame_graph& fg, string::engine_context& ctx, const std::filesystem::path& shader,
    std::shared_ptr<string::dynamic_font_atlas> atlas, string::renderer& rr)
{
    auto s = std::make_shared<shader_scene_state>();
    s->res = declare_resources(fg, scene_viewport(), kSceneSamples, 2048, 0, 256, 256, nullptr);
    s->toy = std::make_unique<shadertoy_pass>(ctx, shader, kSceneSamples);
    auto toy_ui_slot = std::make_shared<std::optional<ui::Ui>>();
    auto toy_panels = std::make_shared<debug::DebugPanels>();
    s->ui = std::make_unique<ui_pass>(
        ctx, kSceneSamples, atlas,
        make_ui_author(std::make_shared<MeshOverlayStats>(), toy_panels, 0, toy_ui_slot),
        make_ui_observer(toy_ui_slot), make_deferred_author(toy_ui_slot));
    s->composite = std::make_unique<string::composite_pass>(ctx, rr.swapchain_format());

    s->toy->declare(fg, s->res.color, s->res.depth);
    s->ui->declare(fg, s->res.color);
    s->composite->declare(fg, s->res.hdr, s->res.swapchain);
    rr.declare_capture(fg, s->res.hdr);

    string::renderer* rp = &rr;
    return [s, rp](float dt) {
        s->toy->tick(dt);
        // The live extent and the slot the NEXT frame records into. Packing slot 0 while recording
        // slot N means most frames draw from a ring nothing filled — flicker, not an error.
        s->ui->tick(dt, rp->extent(), rp->frame_slot());
        s->composite->tick();
    };
}

// The UI-dev sandbox: a gradient background (+ orbit camera + synthetic anchors, both driven by the
// author) and the UI overlay. NO geometry_pass, which is the point — startup is instant because
// nothing loads glTF, builds meshlets or streams textures. Same shape as the shadertoy scene, and
// for the same reason: no post chain, so the composite reads the resolved HDR directly.
struct ui_scene_state
{
    std::unique_ptr<ui_background_pass> bg;
    std::unique_ptr<ui_pass> ui;
    std::unique_ptr<string::composite_pass> composite;
    scene_resources res;
};

void declare_stream_uploads(string::frame_graph& fg, string::engine_context& ctx)
{
    string::TransferBatch* transfer = &ctx.transfer;
    fg.pass("uploads.stream")
      .toggle([transfer] { return transfer->pending(); })
      .transfer([transfer](string::pass_context& pc) { transfer->record(pc.rec); });
}

std::function<void(float)> make_ui_scene(
    string::frame_graph& fg, string::engine_context& ctx,
    std::shared_ptr<string::dynamic_font_atlas> atlas, string::renderer& rr)
{
    const int np = cv_ui_nameplates().get();
    // A visible handful of anchors by default, so nameplates show without opting into the stress.
    const std::uint32_t anchor_count = np > 0 ? static_cast<std::uint32_t>(np) : 24u;
    const std::size_t np_budget = np > 0 ? static_cast<std::size_t>(np) : 0;   // 0 = draw all
    const std::string screen = cv_ui_screen().get();

    auto s = std::make_shared<ui_scene_state>();
    declare_stream_uploads(fg, ctx);
    // SINGLE-SAMPLE, and the UI draws straight into scene.hdr — see the declare block below.
    // `scene.hdr` is the RESOLVE target of the multisampled `scene.color`, and the only thing that
    // ever declares that resolve pair is the geometry pass (`geo->declare(fg, color, hdr, ...)`:
    // two colour writes at different sample counts IS the resolve, per derive_groups). A UI-only
    // scene has no geometry pass, so at kSceneSamples nothing wrote hdr and composite tonemapped an
    // untouched target — the whole scene rendered black.
    //
    // 1x rather than resolving because MSAA earns nothing here: there is no 3D geometry, and both
    // UI shaders antialias themselves analytically (ui_shader's SDF rounded box takes ~1px coverage
    // from smoothstep(-0.5, 0.5, d); text_shader uses a fwidth-wide smoothstep on the glyph
    // distance). 4x would cost a full-res 4x colour + 4x depth to improve edges nothing rasterizes.
    s->res = declare_resources(fg, scene_viewport(), VK_SAMPLE_COUNT_1_BIT, 2048, 0, 256, 256, nullptr);
    s->bg = std::make_unique<ui_background_pass>(ctx, VK_SAMPLE_COUNT_1_BIT);

    // Shared between the author and the post-layout hook: the workspace is authored before layout
    // and observed after it, and both need the same instance.
    auto ws = std::make_shared<string::ui::Workspace>();
    if (screen == "workspace")
        client::seed_workspace(*ws);
    auto ui_slot = std::make_shared<std::optional<ui::Ui>>();
    auto dbg_panels = std::make_shared<debug::DebugPanels>();
    s->ui = std::make_unique<ui_pass>(
        ctx, VK_SAMPLE_COUNT_1_BIT, atlas,
        make_ui_dev_author(std::make_shared<UiScene>(), anchor_count, dbg_panels, np_budget, screen,
                           ws, ui_slot),
        make_ui_observer(ui_slot, ws), make_deferred_author(ui_slot));
    s->composite = std::make_unique<string::composite_pass>(ctx, rr.swapchain_format());

    // Straight into hdr: it is what composite and the capture read, and with no geometry pass in
    // this scene nothing else would ever write it.
    s->bg->declare(fg, s->res.hdr, s->res.depth);
    s->ui->declare(fg, s->res.hdr);
    s->composite->declare(fg, s->res.hdr, s->res.swapchain);
    rr.declare_capture(fg, s->res.hdr);

    string::renderer* rp = &rr;
    return [s, rp](float dt) {
        s->bg->tick(dt);
        s->ui->tick(dt, rp->extent(), rp->frame_slot());
        s->composite->tick();
    };
}

std::function<void(float)> make_geometry_scene(
    string::frame_graph& fg, string::engine_context& ctx, VkExtent2D viewport,
    VkSampleCountFlagBits samples, VkFormat swapchain_format,
    std::vector<std::filesystem::path> models,
    std::shared_ptr<string::dynamic_font_atlas> atlas, std::size_t np_stress, string::renderer& rr,
    bool lookdev)
{
    auto s = std::make_shared<geometry_scene_state>();
    s->graph = &fg;

    s->geo = std::make_unique<geometry_pass>(ctx, samples, std::move(models), s->mesh_stats, lookdev);
    ::string::render::GeometryScene* scene = s->geo->scene();

    const ::string::render::ProbeVolume gi_volume =
        probe_gi_component::fit_volume(scene->scene_aabb_min_, scene->scene_aabb_max_);
    // The app allocates the temporal backings BEFORE declaring, so every persistent handle is
    // backed at compile and its descriptor slots bind once, up front (brief 21 D3).
    s->backing.create(ctx, viewport, scene->cull_max_draws_, gi_volume, s->geo->gi_capture_entries());
    // The work-list sizes come from the meshlet build (which has already run, in the geometry_pass
    // constructor above): both sides read the SAME numbers, so a declaration cannot drift from the
    // layout the shaders index with.
    s->res = declare_resources(fg, viewport, samples, scene->settings_.shadow_resolution,
                               scene->settings_.cascade_count, scene->wl_layout_.size,
                               VkDeviceSize{ sizeof(uint32_t) } * scene->cull_max_draws_,
                               &s->backing);
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
    s->composite    = std::make_unique<string::composite_pass>(ctx, swapchain_format);

    const std::span<const string::gpu::image> cascades{ r.shadow, scene->settings_.cascade_count };
    const probe_gi_component::resources gi_res{
        r.gi_irradiance, r.gi_cap_gbuf, r.gi_cap_albedo, r.gi_visibility,
        r.gi_cube_albedo, r.gi_cube_nd, r.gi_cube_depth,
        r.gi_active, r.gi_offset, r.gi_meshlet_table };

    declare_stream_uploads(fg, ctx);

    s->froxel->declare(fg, r.froxels, r.lights);
    s->ibl->declare(fg, r.env_capture, r.env_prefiltered, r.dfg_lut, r.ibl_sh);
    s->gtao->declare(fg, r.hiz_depth_prev, r.gtao_raw, r.gtao_ao);
    s->gi->declare(fg, gi_res, r.ibl_sh, r.scene_data, cascades, r.color, r.depth);
    s->shadow->declare(fg, cascades, r.worklists);
    s->sky->declare(fg, r.color, r.depth, [] { return ::string::render::cv_pass_sky().get(); });
    s->geo->declare(fg, r.color, r.hdr, r.depth, r.hiz_depth, r.hiz_pyramid,
                    r.worklists, r.scene_data, r.lights, r.stats,
                    cascades, r.gtao_ao, r.env_prefiltered, r.dfg_lut, r.ibl_sh, r.froxels);
    s->transparency->declare(fg, r.color, r.depth, r.transparency_list, r.scene_data, r.stats);
    s->debug_lines->declare(fg, r.color, r.depth);
    s->ui->declare(fg, r.color);
    s->post->declare(fg, r.hdr, r.bloom, r.bloom_mips, r.histogram_bins);
    s->composite->declare(fg, r.hdr, r.swapchain);

    // The headless gates capture the resolved HDR target — an app-declared transient, so the app is
    // what names it. This is the seam the renderer cannot fill on its own.
    // STRING_CAPTURE_SOURCE=shadow<N> redirects the capture at cascade N's raw depth map
    // (grayscale; white = near occluder) — the headless eyes on shadow-map content.
    string::gpu::image capture_target = r.hdr;
    if (const char* csrc = std::getenv("STRING_CAPTURE_SOURCE");
        csrc != nullptr && std::strncmp(csrc, "shadow", 6) == 0)
    {
        const int c = std::atoi(csrc + 6);
        if (c >= 0 && c < static_cast<int>(scene->settings_.cascade_count))
            capture_target = r.shadow[c];
    }
    rr.declare_capture(fg, capture_target);

    // The OWNER half of a resize: re-back the viewport-sized depth-history ring and re-point both
    // handles (identity + rotated-one-back), before the renderer's graph half re-binds. Raw pointer
    // capture: the callback only fires inside the frame loop, while the scene state is alive.
    geometry_scene_state* sp = s.get();
    rr.on_resize([sp](VkExtent2D e) {
        sp->backing.create_hiz_depth(e);
        const std::vector<string::gpu::resource_id>& ring = sp->backing.hiz_depth;
        std::vector<string::gpu::resource_id> rotated(ring.size());
        for (std::size_t i = 0; i < ring.size(); ++i)
            rotated[i] = ring[(i + ring.size() - 1) % ring.size()];
        sp->graph->set_images(sp->res.hiz_depth, ring);
        sp->graph->set_images(sp->res.hiz_depth_prev, rotated);
    });

    // The per-frame tick. Ordinary app code: nothing here resolves a device address or a bindless
    // slot — every one of those is looked up through pass_context while its owning pass records.
    ::string::render::GeometryScene* scene_ptr = scene;
    string::engine_context* ctx_ptr = &ctx;
    string::renderer* rp = &rr;
    return [s, scene_ptr, ctx_ptr, rp](float dt) {
        // The live viewport. This used to arrive via Pass::resize(); with that gone, the scene state
        // carries it and the app is what knows it. Zero here means every viewport-derived layout —
        // the UI most visibly — computes against a 0x0 screen and draws nothing.
        scene_ptr->screen_size = rp->extent();
        const std::uint32_t slot = rp->frame_slot();
        s->geo->tick(dt, slot);
        s->gtao->tick(rp->extent(), static_cast<uint16_t>(slot));
        s->ibl->tick(scene_ptr->ibl_lighting(), false);
        s->gi->tick();
        s->froxel->tick(scene_ptr->froxel_params());
        s->sky->tick(scene_ptr->sky_params());
        s->ui->tick(dt, rp->extent(), slot);
        s->post->tick(dt);
        s->composite->tick();
    };
}
}  // namespace

string::Application::scene_fn build_demo_scene(const std::filesystem::path& resources_dir)
{
    // The UI's SDF atlas is baked once, up front and shared: the UI pass's per-frame layout measures
    // against it and draws text from it.
    const std::vector<std::uint8_t> ttf = read_file(resources_dir / "assets/fonts/DejaVuSans.ttf");
    auto atlas = std::make_shared<string::dynamic_font_atlas>(ttf);

    // Content defaults to the resources dir's assets/. Must precede any ContentRoot::get(), which
    // memoises on first call.
    string::ContentRoot::set_default(resources_dir / "assets");

    const std::size_t np_stress = static_cast<std::size_t>(std::max(0, cv_ui_nameplates().get()));

    string::SceneRegistry& registry = string::SceneRegistry::instance();

    // Registration order is the order the Scene panel lists them in.
    registry.add("ui", "UI sandbox — no geometry, instant start",
                 [atlas](string::frame_graph& fg, string::engine_context& ctx, string::renderer& r)
                   -> std::function<void(float)> {
        return make_ui_scene(fg, ctx, atlas, r);
    });

    // Brief 07: the standing material-probe scene — a roughness x metallic sphere grid plus a
    // white/mirror pair, generated in-process (no glTF load; near-instant startup). Same
    // geometry_pass, so sun/TOD scrub keys, the furnace CVar, IBL, shadows and every capture lever
    // work identically.
    //
    // Brief 09: lookdev PINS MANUAL exposure by default — it is the EV100/material calibration
    // reference, and auto-metering a sphere grid over grey would defeat that. set_env_default, so
    // an explicit STRING_EXPOSURE_AUTO from the user still wins.
    ::string::core::set_env_default("STRING_EXPOSURE_AUTO", "0");
    registry.add("lookdev", "Material probe grid — roughness x metallic",
                 [atlas](string::frame_graph& fg, string::engine_context& ctx, string::renderer& r)
                   -> std::function<void(float)> {
        return make_geometry_scene(fg, ctx, scene_viewport(), kSceneSamples, r.swapchain_format(),
                                   {}, atlas, /*np_stress=*/0, r, /*lookdev=*/true);
    });

    // Sponza is not registered in code: it is `assets/sponza.scene.json`, discovered by the same
    // scan as anything the user drops in. Deliberate — it is the proof the data path is real rather
    // than a second-class route beside a hardcoded one. The descriptor ships; the assets it names
    // stay gitignored, and a descriptor whose files are absent is simply skipped.
    register_content_scenes(registry, atlas, static_cast<int>(np_stress));

    // The same scan as the rescan hook, so "Rescan content" in the Scene menu picks up an asset
    // dropped in while the engine is running. instance() rather than the reference above: that
    // reference dies with this function even though it binds a singleton.
    registry.set_rescan([atlas, np_stress] {
        register_content_scenes(string::SceneRegistry::instance(), atlas,
                                static_cast<int>(np_stress));
    });

    // Texture cook. Lives here rather than in the engine because it needs the asset-tools library,
    // and string-core deliberately does not link it. Blocking — see cook_scene_textures_for.
    registry.set_cook([resources_dir](const string::SceneRegistry::Scene& scene) {
        const uint32_t chunk_budget =
            static_cast<uint32_t>(std::max(0, ::string::render::cv_chunk_budget().get()));
        STRING_LOG_INFO("[cook] '{}': {} asset(s) — the engine will be unresponsive until this "
                        "finishes", scene.name, scene.assets.size());
        ::string::render::cook_scene_textures_for(resources_dir, scene.assets, chunk_budget);
    });

    // `dbg.scene` (STRING_SCENE) is a LOOKUP, not a branch. An unknown name falls back rather than
    // failing: a typo should cost you the scene you asked for, not the session.
    //
    // The fallback chain ends at "ui" because it is the only scene guaranteed to exist — it needs no
    // assets at all. Falling back to sponza was what made an assetless clone unable to start.
    const std::string requested = cv_scene().get();
    const string::SceneRegistry::Scene* selected = registry.find(requested);
    if (selected == nullptr)
    {
        // "demo" is the historical default value of the cvar and means "sponza if you have it".
        if (requested == "demo")
        {
            selected = registry.find("sponza");
        }
        else if (!requested.empty())
        {
            STRING_LOG_WARN("Unknown scene '{}' — is it under the content root ({})?",
                            requested, string::ContentRoot::get().string());
        }
        if (selected == nullptr)
        {
            selected = registry.find("ui");
        }
    }
    registry.set_active(selected->name);
    STRING_LOG_INFO("[scene] '{}' selected", selected->name);

    // Brief 20: the app hands back ONE callback. It constructs the scene against the ready graph and
    // engine context, declares every pass, and returns the per-frame tick. There is no pass list
    // alongside it — that second list, and the unenforced agreement between the two, is what this
    // brief deleted.
    return selected->configure;
}

}  // namespace sandbox
