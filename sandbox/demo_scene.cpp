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
#include <string/render/scene_bridge.hpp>
#include <string/render/scene_uniforms.hpp>
#include <string/render/forward_renderer.hpp>
#include <string/scene/asset_registry.hpp>
#include <string/scene/world.hpp>
#include <string/asset/tools/scene_cook.hpp>
#include "content_tools.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

namespace sandbox
{
// The SCENE-state keys the app binds against world.env() (interned once; the geometry pass keeps
// only its renderer-debug keys).
namespace scene_actions
{
inline constexpr ::string::ActionId sun_animate   = ::string::action_id("sun_animate");
inline constexpr ::string::ActionId time_back     = ::string::action_id("time_back");
inline constexpr ::string::ActionId time_fwd      = ::string::action_id("time_fwd");
inline constexpr ::string::ActionId toggle_lights = ::string::action_id("toggle_lights");
}  // namespace scene_actions
}  // namespace sandbox

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


// Defined below; the content-scan registry lambdas above call them. The forward technique stack
// (scene_resources / declare_resources / declare_stream_uploads / forward_renderer) is the
// RENDERER library's now.
using ::string::render::scene_resources;
using ::string::render::scene_backing;
using ::string::render::declare_resources;
using ::string::render::declare_stream_uploads;
using ::string::render::forward_renderer;
std::function<void(float)> make_geometry_scene(
    string::frame_graph& fg, string::engine_context& ctx, VkExtent2D viewport,
    VkSampleCountFlagBits samples, VkFormat swapchain_format,
    std::vector<std::filesystem::path> models,
    std::shared_ptr<string::dynamic_font_atlas> atlas, std::size_t np_stress, string::renderer& rr,
    bool lookdev = false);
// (declare_stream_uploads moved to the renderer library with the preset.)
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
// The debug shell does not own a Ui, so it is not passed here — see DebugPanels::update_and_author.
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
                              std::size_t nameplate_stress, SharedUi fluent,
                              const ::string::scene::world* world = nullptr)
{
    return [mesh_stats, world,
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
        panels->set_inspector([&](ui::Ui& p) { render_dbg.inspector(p, mesh_stats.get(), world); });
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


// The geometry scene, post-preset: load content, spawn it, hand the world to forward_renderer,
// and keep only what is genuinely the APP's — input bindings, the camera/environment drive, the
// crowd/clip demo drivers. The component zoo, the persistent backing and the load-bearing
// declaration order all live inside the preset now (see forward_renderer.cpp for why the order
// matters).

struct geometry_scene_state
{
    // The asset registry (declared before the renderer so it outlives it: passes upload from
    // non-owning views of its heaps). Per-scene for now; the application-level registry that
    // survives scene switches arrives with the public-API step.
    tools_cook_provider cook;
    std::unique_ptr<string::assets::registry> assets;
    // The WORLD: what exists, where. The app's half of the scene-layer split — everything GPU-side
    // (bridge, backing, all thirteen passes, their declaration order) is the preset's now.
    std::unique_ptr<string::scene::world> world;
    std::vector<string::scene::entity> base_entities;    // one per loaded asset, spawn order
    std::vector<string::scene::entity> crowd_entities;   // the K-toggle stress grid
    // dbg.orbit motion lever state (drives world.camera() for headless disocclusion captures).
    bool orbit_latched = false;
    glm::vec3 orbit_pos{ 0.0f };
    float orbit_yaw = 0.0f;
    float orbit_pitch = 0.0f;
    float orbit_phase = 0.0f;
    // demo clip-driver state (brief 23: the state machine is game code)
    std::string active_clip;
    float demo_timer = 0.0f;
    bool demo_walking = false;
    std::unique_ptr<forward_renderer> fr;
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
    // The composite is constructed before the UI pass, which holds it for its exposure pre-divide.
    s->composite = std::make_unique<string::composite_pass>(ctx, rr.swapchain_format());
    s->ui = std::make_unique<ui_pass>(
        ctx, kSceneSamples, s->composite.get(), atlas,
        make_ui_author(std::make_shared<MeshOverlayStats>(), toy_panels, 0, toy_ui_slot),
        make_ui_observer(toy_ui_slot), make_deferred_author(toy_ui_slot));

    s->toy->declare(fg, s->res.color, s->res.depth);
    s->ui->declare(fg, s->res.color);
    s->composite->declare(fg, s->res.hdr, s->res.swapchain);
    rr.declare_capture(fg, s->res.hdr, s->composite.get());

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
    // The composite is constructed before the UI pass, which holds it for its exposure pre-divide.
    s->composite = std::make_unique<string::composite_pass>(ctx, rr.swapchain_format());
    s->ui = std::make_unique<ui_pass>(
        ctx, VK_SAMPLE_COUNT_1_BIT, s->composite.get(), atlas,
        make_ui_dev_author(std::make_shared<UiScene>(), anchor_count, dbg_panels, np_budget, screen,
                           ws, ui_slot),
        make_ui_observer(ui_slot, ws), make_deferred_author(ui_slot));

    // Straight into hdr: it is what composite and the capture read, and with no geometry pass in
    // this scene nothing else would ever write it.
    s->bg->declare(fg, s->res.hdr, s->res.depth);
    s->ui->declare(fg, s->res.hdr);
    s->composite->declare(fg, s->res.hdr, s->res.swapchain);
    rr.declare_capture(fg, s->res.hdr, s->composite.get());

    string::renderer* rp = &rr;
    return [s, rp](float dt) {
        s->bg->tick(dt);
        s->ui->tick(dt, rp->extent(), rp->frame_slot());
        s->composite->tick();
    };
}

// The crowd stress scene as CONTENT: the K toggle (or r.crowd.enabled) spawns/despawns a
// kCrowdGrid x kCrowdGrid grid of copies of every base entity around the origin cell.
void toggle_crowd(geometry_scene_state& s, bool on)
{
    constexpr uint32_t kCrowdGrid = 6;
    if (!on)
    {
        for (const string::scene::entity e : s.crowd_entities) s.world->despawn(e);
        s.crowd_entities.clear();
        return;
    }
    if (!s.crowd_entities.empty()) return;
    const glm::vec3 extent = s.fr->bridge().bounds_max() - s.fr->bridge().bounds_min();
    const float spacing_x = extent.x * 1.1f;
    const float spacing_z = extent.z * 1.1f;
    const int side = static_cast<int>(kCrowdGrid);
    for (int gx = 0; gx < side; ++gx)
        for (int gz = 0; gz < side; ++gz)
        {
            const int cx = gx - side / 2;
            const int cz = gz - side / 2;
            if (cx == 0 && cz == 0) continue;   // the base scene occupies the origin cell
            const glm::mat4 offset = glm::translate(
                glm::mat4(1.0f), glm::vec3(static_cast<float>(cx) * spacing_x, 0.0f,
                                           static_cast<float>(cz) * spacing_z));
            for (const string::scene::entity base : s.base_entities)
            {
                s.crowd_entities.push_back(s.world->spawn(
                    { .asset = s.world->asset_of(base), .transform = offset,
                      .debug_name = "crowd" }));
            }
        }
    STRING_LOG_INFO("[crowd] {} entities spawned ({} base x {} grid cells)",
                    s.crowd_entities.size(), s.base_entities.size(), kCrowdGrid * kCrowdGrid);
}

// dbg.orbit (STRING_ORBIT): sway the camera around a latched base pose so headless captures
// exercise per-frame disocclusion. Applied AFTER Camera::update so it overrides input.
void drive_orbit(geometry_scene_state& s, string::engine_context& ctx, float dt, float aspect)
{
    const float orbit_speed = ::string::render::cv_orbit().get();
    if (orbit_speed == 0.0f) return;
    string::Camera& cam = s.world->camera();
    if (!s.orbit_latched)
    {
        s.orbit_pos = cam.position();
        s.orbit_yaw = cam.yaw();
        s.orbit_pitch = cam.pitch();
        s.orbit_latched = true;
    }
    s.orbit_phase += orbit_speed * dt;
    const float radius = 0.35f;
    const float yaw_amp = 0.06f;
    const float pitch_amp = 0.03f;
    const glm::vec3 pos = s.orbit_pos
        + glm::vec3(std::cos(s.orbit_phase) * radius,
                    std::sin(s.orbit_phase * 0.5f) * radius * 0.4f,
                    std::sin(s.orbit_phase) * radius);
    cam.set_pose(pos, s.orbit_yaw + std::sin(s.orbit_phase) * yaw_amp,
                 s.orbit_pitch + std::sin(s.orbit_phase * 0.7f) * pitch_amp);
    cam.update(ctx.input_map, 0.0f, aspect);
}

// The scene keys (T/[/]/L) + the headless TOD/furnace levers, applied to world.env(). Brief 07:
// the r.tod pin wins over keys/animation so captures are deterministic.
void drive_environment(geometry_scene_state& s, string::engine_context& ctx, float dt)
{
    string::scene::environment& env = s.world->env();
    env.sun_lean = ::string::render::cv_sun_lean().get();
    if (ctx.input_map.pressed(scene_actions::sun_animate))
    {
        env.animate_sun = !env.animate_sun;
        STRING_LOG_INFO("Time-of-day {}", env.animate_sun ? "ANIMATING" : "paused");
    }
    if (ctx.input_map.held(scene_actions::time_back))
        env.time_of_day = glm::clamp(env.time_of_day - dt * 0.15f, 0.0f, 1.0f);
    if (ctx.input_map.held(scene_actions::time_fwd))
        env.time_of_day = glm::clamp(env.time_of_day + dt * 0.15f, 0.0f, 1.0f);
    if (ctx.input_map.pressed(scene_actions::toggle_lights))
    {
        env.lights_enabled = !env.lights_enabled;
        STRING_LOG_INFO("Local lights {}", env.lights_enabled ? "ON" : "OFF");
    }
    if (const float tod = ::string::render::cv_time_of_day().get(); tod >= 0.0f)
        env.time_of_day = glm::clamp(tod, 0.0f, 1.0f);
    env.furnace = ::string::render::cv_furnace().get();
}

// Brief 23: the demo's clip driver — anim.clip requests, else the auto idle<->walk demo. GAME
// code by design (the engine samples + blends; this decides WHAT plays).
void drive_clip_demo(geometry_scene_state& s, float dt)
{
    s.world->set_animation_rate(cv_anim_rate().get());
    const float fade_seconds = std::max(0.0f, cv_anim_blend().get());
    const std::string request = cv_anim_clip().get();
    if (request != s.active_clip)
    {
        s.active_clip = request;
        if (!request.empty())
            for (const string::scene::entity e : s.base_entities)
                if (!s.world->play(e, request, fade_seconds)
                    && !s.world->animators_of(e).empty())
                    STRING_LOG_WARN("[anim] no clip '{}' in this entity's pack", request);
    }
    if (request.empty() && cv_anim_demo().get() != 0)
    {
        s.demo_timer += dt;
        if (s.demo_timer >= 3.0f)   // idle <-> walk cadence
        {
            s.demo_timer = 0.0f;
            s.demo_walking = !s.demo_walking;
            for (const string::scene::entity e : s.base_entities)
                s.world->play(e, s.demo_walking ? "Walk_Loop" : "Idle_Loop", fade_seconds);
        }
    }
}

std::function<void(float)> make_geometry_scene(
    string::frame_graph& fg, string::engine_context& ctx, VkExtent2D viewport,
    VkSampleCountFlagBits samples, VkFormat swapchain_format,
    std::vector<std::filesystem::path> models,
    std::shared_ptr<string::dynamic_font_atlas> atlas, std::size_t np_stress, string::renderer& rr,
    bool lookdev)
{
    auto s = std::make_shared<geometry_scene_state>();

    // Load this scene's content into the app-owned asset registry BEFORE any renderer object
    // exists (asset-layer split): models via the cooked path (cook-on-load through the injected
    // tools provider), generated content via bake_scene + load_baked. Order matters for parity —
    // models first, then the synthetic transparency test set, exactly like the old in-pass merge.
    s->assets = std::make_unique<string::assets::registry>(
        ctx,
        string::assets::registry_config{
            .chunk_budget =
                static_cast<uint32_t>(std::max(0, ::string::render::cv_chunk_budget().get())) },
        &s->cook);
    std::vector<string::assets::asset_id> loaded;
    if (lookdev)
    {
        loaded.push_back(load_lookdev_asset(*s->assets));
    }
    else
    {
        for (const std::filesystem::path& m : models)
        {
            const string::assets::asset_id id = s->assets->load(ctx.resources_path / m);
            if (id.valid()) loaded.push_back(id);
        }
    }
    if (::string::render::cv_transp_test().get())
    {
        const string::assets::asset_id id = load_transp_test_asset(*s->assets);
        if (id.valid()) loaded.push_back(id);
    }
    // The world: one entity per loaded asset, identity placement, in load order — which is what
    // keeps the bridge's row order identical to the old merged draw order (byte parity).
    s->world = std::make_unique<string::scene::world>(*s->assets);
    for (const string::assets::asset_id id : loaded)
    {
        const string::assets::asset* a = s->assets->get(id);
        s->base_entities.push_back(s->world->spawn(
            { .asset = id, .debug_name = a != nullptr ? a->name() : std::string_view{} }));
    }

    // The preset: registry heaps, bridge, backing, all thirteen passes, the capture target and
    // the resize hook. The app hands it the world + its UI closures and keeps nothing
    // renderer-shaped. Budgets stay defaulted: base content x the 6x6 crowd grid, which is
    // exactly what this demo can spawn.
    auto ui_slot = std::make_shared<std::optional<ui::Ui>>();
    auto dbg_panels = std::make_shared<debug::DebugPanels>();
    s->fr = std::make_unique<forward_renderer>(
        ctx, rr, *s->assets, *s->world,
        forward_renderer::settings{ .samples = samples, .viewport = viewport,
                                    .swapchain_format = swapchain_format, .lookdev = lookdev });
    s->fr->set_ui({ atlas,
                    make_ui_author(s->fr->overlay_stats(), dbg_panels, np_stress, ui_slot,
                                   s->world.get()),
                    make_ui_observer(ui_slot), make_deferred_author(ui_slot) });
    s->fr->declare(fg);

    s->fr->set_crowd_hook([sp = s.get()](bool on) { toggle_crowd(*sp, on); });
    // The camera + environment are the WORLD's now: frame it, bind the fly controls + the scene
    // keys (T/[/]/L), and apply the headless pose/animation levers. The app owns every input
    // binding for SCENE state; the geometry pass keeps only its renderer-debug keys.
    string::Camera::bind_default_controls(ctx.input_map);
    ctx.input_map.bind_button("sun_animate", string::KeyCode::T);
    ctx.input_map.bind_button("time_back", string::KeyCode::LEFT_BRACKET);
    ctx.input_map.bind_button("time_fwd", string::KeyCode::RIGHT_BRACKET);
    ctx.input_map.bind_button("toggle_lights", string::KeyCode::L);
    s->world->camera().frame_bounds(s->fr->bridge().bounds_min(), s->fr->bridge().bounds_max());
    // STRING_CAM="px,py,pz,yaw,pitch" (radians); empty CVar = keep the scene-framed default pose.
    if (const std::string cam = ::string::render::cv_camera_pose().get(); !cam.empty())
    {
        glm::vec3 pos{}; float yaw = 0.0f, pitch = 0.0f;
        if (std::sscanf(cam.c_str(), "%f,%f,%f,%f,%f", &pos.x, &pos.y, &pos.z, &yaw, &pitch) == 5)
            s->world->camera().set_pose(pos, yaw, pitch);
    }
    s->world->env().animate_sun = ::string::render::cv_sun_animate().get();
    // The lookdev probe scene reads material response under sun + sky IBL only — the local-light
    // stress set would pollute it (L / STRING_LIGHTS=1 still re-enable it explicitly).
    if (lookdev && std::getenv("STRING_LIGHTS") == nullptr)
        s->world->env().lights_enabled = false;

    // The per-frame tick. Ordinary app code: nothing here resolves a device address or a bindless
    // slot — every one of those is looked up through pass_context while its owning pass records.
    string::renderer* rp = &rr;
    string::engine_context* cp = &ctx;
    return [s, rp, cp](float dt) {
        const VkExtent2D ext = rp->extent();
        // The live viewport, pushed into the world (the app is what knows the window). Zero here
        // means every viewport-derived layout — the UI most visibly — draws nothing.
        s->world->set_viewport(ext.width, ext.height);

        // The app drives the world's camera + environment + clip choice (scene-layer split).
        const float aspect = ext.height == 0 ? 1.0f : ext.width / static_cast<float>(ext.height);
        s->world->camera().update(cp->input_map, dt, aspect);
        drive_orbit(*s, *cp, dt, aspect);
        drive_environment(*s, *cp, dt);
        drive_clip_demo(*s, dt);

        // Scene-layer order: the world flushes its hierarchy/sun/animators + snapshot, THEN the
        // preset re-derives its GPU mirror and ticks every pass against the current frame.
        s->world->tick(dt);
        s->fr->tick(dt);
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
        ::string::asset::tools::cook_scene_textures_for(resources_dir, scene.assets, chunk_budget);
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
