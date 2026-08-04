#include <string/application.hpp>
#include <string/vulkan/scene_registry.hpp>
#include <string/core/signals.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>
#include <thread>

namespace String {

void Application::initialize(const ApplicationInfo& info, const RenderPlan& plan)
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
    renderer_ = std::make_unique<Renderer>(info, window_, plan);
}

Application::~Application()
{
    renderer_.reset();
    window_.reset();
    event_handler_.sink<WindowEvent>().disconnect();
}

void Application::run() {
    const auto start = std::chrono::steady_clock::now();
    // Cap at 60fps
    const auto period = std::chrono::duration<double>(1 / 60);

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

        // Scene switching happens HERE — between frames, never inside one. A UI click or console
        // command only records a request (SceneRegistry::request); this is the point where no
        // command buffer is being recorded and the passes about to be destroyed are not in use by
        // anything except already-submitted work, which load_scene() waits out.
        if (SceneRegistry::instance().has_pending())
        {
            SceneRegistry& registry = SceneRegistry::instance();
            if (const SceneRegistry::Scene* scene = registry.take_pending())
            {
                RenderPlan plan;
                plan.configure(scene->configure);
                renderer_->load_scene(plan);
                registry.set_active(scene->name);
            }
        }

        if (!freeze_rendering_)
            renderer_->draw();

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

}  // namespace String
