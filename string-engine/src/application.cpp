#include <string/application.hpp>
#include <string/core/signals.hpp>
#include <thread>

namespace String {

void Application::initialize(const ApplicationInfo& info, const RenderPlan& plan)
{
    event_handler_.sink<WindowEvent>().connect<&Application::on_window_event>(*this);
    window_ = std::make_shared<Window>(Window::Properties{.title = info.application_name, .extent = {800, 800}});
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
