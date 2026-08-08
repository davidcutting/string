#include <string/application.hpp>
#include <string/vulkan/scene_registry.hpp>
#include <string/vulkan/content_root.hpp>
#include <string/core/cvar.hpp>
#include <string/core/logger.hpp>
#include <string/core/signals.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>
#include <thread>

namespace string {

void Application::initialize(const ApplicationInfo& info, scene_fn scene)
{
    event_handler_.sink<WindowEvent>().connect<&Application::on_window_event>(*this);
    // STRING_WINDOW_SIZE=WxH overrides the initial window size — headless repro tooling: bugs can
    // be resolution/timing dependent (window managers also resize real sessions at map time), so
    // captures must be able to match a user's actual resolution, not just the 800x800 default.
    View::Extent extent = { 800, 800 };
    if (const char* ws = std::getenv("STRING_WINDOW_SIZE"))
    {
        unsigned w = 0, h = 0;
        if (std::sscanf(ws, "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 16384 && h <= 16384)
            extent = { w, h };
    }
    window_ = std::make_shared<Window>(Window::Properties{.title = info.application_name, .extent = extent});
    init_signal_handling();
    renderer_ = std::make_unique<string::renderer>(info, window_);

    // Brief 20: the application authors its graph ONCE, here, and compiles it ONCE. Nothing after
    // this point re-authors or re-plans — a toggle is an in-graph conditional and a resize is a
    // backing swap, which is what makes an author callback unnecessary.
    tick_ = scene(graph_, renderer_->context(), *renderer_);
    // Everything the scene staged while it was being built goes to the GPU now, once, so the first
    // frame does not carry a scene's worth of staging (brief 21 step 5). Per-frame uploads are the
    // declared pass the scene authored, not this.
    renderer_->flush_construction_uploads();
    frame_ = graph_.compile(renderer_->context(), renderer_->extent());
    renderer_->publish_introspection(frame_);

    // Window resizes are LATCHED here and applied between frames (see run()): a resize mid-record
    // would swap backing under a command buffer. Only the latest extent matters.
    window_->register_resize_event_callback([this](const View::Extent& e) { pending_resize_ = e; });
}

// Tear the current scene down and stand the next one up. Brief 20 made the APP own the graph, so a
// switch is not "hand the renderer a new plan" any more — it is the whole of initialize()'s author →
// compile → publish sequence run again against a fresh frame_graph.
//
// The ORDER is the entire difficulty, because ownership is split three ways:
//   1. device idle — everything below frees GPU objects that submitted work may still be reading;
//   2. frame_.release() — the compiled frame gives back its bindless slots, cached views and
//      transient backing. It must go BEFORE the scene dies, because those slots name resources the
//      scene owns, and unbinding after they are freed is a use-after-free in the descriptor table;
//   3. drop the renderer's callbacks into the OLD scene — on_resize captures scene state by raw
//      pointer (it must; the callback only fires inside the frame loop while that state is alive),
//      so leaving it installed across a switch leaves a dangling call waiting for the next resize;
//   4. tick_ = nullptr — releases the scene's pass objects and every persistent they own;
//   5. a FRESH graph. Reusing it would keep the old scene's declarations, and a switch that
//      silently kept stale declarations is the exact class of bug brief 20 exists to remove.
// Only then can the new scene author into a clean graph.
void Application::load_scene(const scene_fn& configure, std::string_view name)
{
    STRING_LOG_INFO("[scene] switching to '{}'", name);

    // COMMIT THE OUTGOING SCENE'S STAGED UPLOADS FIRST, while the images they name are still alive.
    // The transfer batch is an engine service that outlives any scene: it accumulates copies until
    // something records them. A scene torn down with uploads still pending (a texture whose stream
    // landed after the last flush) leaves entries pointing at images the teardown then destroys, and
    // the NEXT scene's flush walks that same list and resolves a freed id — which is what
    // `no live image with id 57` was. Flushing here empties the list against live resources, so
    // teardown cannot strand anything in it.
    renderer_->flush_construction_uploads();

    renderer_->wait_idle();

    frame_.release(renderer_->context());
    renderer_->on_resize(nullptr);
    tick_ = nullptr;
    graph_.clear();

    tick_ = configure(graph_, renderer_->context(), *renderer_);
    renderer_->flush_construction_uploads();
    frame_ = graph_.compile(renderer_->context(), renderer_->extent());
    renderer_->publish_introspection(frame_);

    SceneRegistry::instance().set_active(name);
    STRING_LOG_INFO("[scene] '{}' active", name);
}

Application::~Application()
{
    // ORDER IS LOAD-BEARING. The application owns its pass objects (captured by the tick callback and
    // by the graph's recording callbacks), and every one of them frees GPU resources through the
    // renderer's allocator in its destructor. So the scene must die BEFORE the renderer: dropping the
    // renderer first destroys the allocator out from under passes that are still about to use it.
    //
    // This is the cost of the app owning the graph — the lifetime that used to be the renderer's
    // problem is now stated here, once, explicitly.
    // Drain first: the pass destructors below free pipelines and images that submitted command
    // buffers still reference, and this is the only place that waits for them.
    if (renderer_) renderer_->wait_idle();
    // The pass objects live in the tick callback's captures and in the graph's recording callbacks;
    // dropping both is what releases them.
    tick_ = nullptr;
    frame_ = {};
    renderer_.reset();
    window_.reset();
    event_handler_.sink<WindowEvent>().disconnect();
}

// STRING_RESIZE_AT=frame:WxH — drive a resize from the frame counter instead of a window event.
// Resize is the least-testable path in the renderer: an offscreen SDL window never emits one, so
// nothing headless could reach it at all, and it is precisely where stale backing, leaked transients
// and unbound descriptors surface. Answers the new extent exactly once, on the nominated frame.
namespace
{
std::optional<View::Extent> scripted_resize()
{
    static unsigned at = 0, w = 0, h = 0;
    static const bool parsed = [] {
        if (const char* s = std::getenv("STRING_RESIZE_AT")) std::sscanf(s, "%u:%ux%u", &at, &w, &h);
        return true;
    }();
    (void)parsed;
    static std::uint64_t frame = 0;
    static bool fired = false;
    ++frame;
    if (fired || at == 0 || frame < at || w < 64 || h < 64) return std::nullopt;
    fired = true;
    STRING_LOG_INFO("[resize] STRING_RESIZE_AT: {}x{} at frame {}", w, h, frame);
    return View::Extent{ w, h };
}

// STRING_MAXIMIZE_AT=frame — ask the WINDOW MANAGER to maximize on the nominated frame. Distinct
// from STRING_RESIZE_AT, which calls renderer::resize directly and never touches the window: that
// leaves the SDL window at its old size and only exercises the renderer half. Maximizing goes
// through the platform, so the size change comes back as a real window event with compositor-chosen
// geometry — which is the reported trigger and the one path nothing headless could reach.
// STRING_RESIZE_STORM=<frames> — drag-resize simulation: a new window width every frame, through
// the platform, for `frames` frames. A drag emits a continuous stream of window events, which is a
// materially different path from one resize at rest: the swapchain goes OUT_OF_DATE repeatedly and
// the presenter's own recreate path runs between the renderer's. Nothing else could reach it.
std::optional<View::Extent> scripted_storm()
{
    static unsigned frames = 0;
    static const bool parsed = [] {
        if (const char* s = std::getenv("STRING_RESIZE_STORM")) std::sscanf(s, "%u", &frames);
        return true;
    }();
    (void)parsed;
    static std::uint64_t frame = 0;
    if (frames == 0 || ++frame > frames) return std::nullopt;
    // Deterministic, non-monotonic widths so the sequence both grows and shrinks (a shrink reuses
    // memory a grow does not). Height fixed, matching a horizontal drag.
    static const unsigned widths[] = { 900, 1338, 1000, 1351, 1180, 1415, 960, 1370, 1024, 1391 };
    return View::Extent{ widths[frame % (sizeof(widths) / sizeof(widths[0]))], 800 };
}

bool scripted_maximize()
{
    static unsigned at = 0;
    static const bool parsed = [] {
        if (const char* s = std::getenv("STRING_MAXIMIZE_AT")) std::sscanf(s, "%u", &at);
        return true;
    }();
    (void)parsed;
    static std::uint64_t frame = 0;
    static bool fired = false;
    ++frame;
    if (fired || at == 0 || frame < at) return false;
    fired = true;
    STRING_LOG_INFO("[resize] STRING_MAXIMIZE_AT: maximizing at frame {}", frame);
    return true;
}
}  // namespace

void Application::run() {
    const auto start = std::chrono::steady_clock::now();
    // Cap at 60fps.
    //
    // 1.0 / 60.0, NOT 1 / 60 — the latter is integer division and yields a ZERO period, which makes
    // `(now - start) / period` a divide-by-zero (+inf), `(inf + 1) * period` a NaN, and the NaN ->
    // integral-duration conversion in `start + ...` undefined. libstdc++'s sleep_until shrugs and
    // returns immediately (symptom: no frame cap, invisible); MSVC's STL spins in its retry loop
    // forever and the app hangs at 100% CPU immediately after frame 1 presents.
    const auto period = std::chrono::duration<double>(1.0 / 60.0);

    while (application_running_ && !g_signal_quit.load())
    {
        window_->update(event_handler_);

        event_handler_.update();

        // STRING_SCENE_SWITCH="name@frame[,name@frame...]" drives scene switches headlessly, since a
        // menu click is not reachable without a window. Kept permanently (same species as
        // STRING_UI_NO_SKIP): it is the only way to gate the load_scene teardown path, and that path
        // is where resource-ownership bugs surface — see the resource_id-0 aliasing bug it caught.
        {
            static const char* spec = std::getenv("STRING_SCENE_SWITCH");
            static std::uint64_t frame = 0;
            if (spec != nullptr)
            {
                std::string s(spec);
                std::size_t pos = 0;
                while (pos < s.size())
                {
                    const std::size_t comma = std::min(s.find(',', pos), s.size());
                    const std::string entry = s.substr(pos, comma - pos);
                    const std::size_t at = entry.find('@');
                    if (at != std::string::npos &&
                        std::strtoull(entry.c_str() + at + 1, nullptr, 10) == frame)
                    {
                        SceneRegistry::instance().request(entry.substr(0, at));
                    }
                    pos = comma + 1;
                }
            }
            ++frame;
        }

        // `content.root <dir>` from the console: validate + persist the user's content folder.
        // Polled rather than hooked because CVar has no change callback; a string compare per frame
        // is free next to the work below. Applying it live would mean re-registering every scene
        // while one is loaded, so set() deliberately only persists and asks for a restart.
        {
            static std::string last_seen = cv_content_root().get();
            if (const std::string now = cv_content_root().get(); now != last_seen)
            {
                last_seen = now;
                if (!now.empty())
                {
                    if (std::string err; !ContentRoot::set(now, err))
                    {
                        STRING_LOG_WARN("[content] {}", err);
                    }
                }
            }
        }

        // `content.cook <scene>` — the same texture cook the Scene menu offers, reachable from the
        // console and headlessly (a menu click is not, which is how this gets gated). Blocking, by
        // design; cleared after firing so it runs once per request rather than every frame.
        {
            static string::core::CVar<std::string> cook_scene{
                "content.cook", "",
                "cook a scene's textures to KTX2/BC7 (blocking, minutes). Name, or empty for none."};
            // Seeded from the env ONCE, read directly rather than via the cvar's env alias: this
            // cvar is constructed on the first poll, which is after the registry's env sweep, so
            // the alias would never see it (same trap as ContentRoot).
            static const bool seeded = [] {
                if (const char* e = std::getenv("STRING_CONTENT_COOK"); e != nullptr && *e != '\0')
                {
                    cook_scene.set(e);
                }
                return true;
            }();
            (void)seeded;
            if (const std::string want = cook_scene.get(); !want.empty())
            {
                cook_scene.set("");
                SceneRegistry::instance().cook(want == "*" ? SceneRegistry::instance().active()
                                                          : std::string_view{ want });
            }
        }

        // Scene switching happens HERE — between frames, never inside one. A UI click or console
        // command only records a request (SceneRegistry::request); this is the point where no
        // command buffer is being recorded and the passes about to be destroyed are not in use by
        // anything except already-submitted work, which the device-idle wait below flushes.
        if (const SceneRegistry::Scene* next = SceneRegistry::instance().take_pending())
        {
            load_scene(next->configure, next->name);
        }

        if (const std::optional<View::Extent> forced = scripted_resize()) pending_resize_ = *forced;
        if (scripted_maximize()) window_->maximize();
        if (const std::optional<View::Extent> storm = scripted_storm()) window_->set_size(*storm);

        // Apply a latched window resize between frames — no command buffer is recording and the
        // renderer waits the device idle before swapping any backing.
        if (pending_resize_)
        {
            renderer_->resize(frame_, *pending_resize_);
            pending_resize_.reset();
        }

        if (!freeze_rendering_)
        {
            const float dt = static_cast<float>(period.count());
            if (tick_) tick_(dt);              // ordinary app code, before the graph runs
            renderer_->render_frame(frame_, dt);
        }

        const auto now = std::chrono::steady_clock::now();
        const auto iterations = (now - start) / period;
        const auto next_start = start + (iterations + 1) * period;

        std::this_thread::sleep_until(next_start);
    }
}

void Application::close()
{
    application_running_ = false;
}

void Application::on_window_event(const WindowEvent& event)
{
    if (event.close)
    {
        application_running_ = false;
        freeze_rendering_ = true;
    }
    else if (event.minimize)
        freeze_rendering_ = true;
    else if (event.maximize)
        freeze_rendering_ = false;
}

}  // namespace string
