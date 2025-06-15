#include <string/application.hpp>

namespace String {

void Application::initialize(const ApplicationInfo& info) {
    String::Logger::initialize();

    window_ = std::make_shared<Window>(Window::Properties{.title = info.application_name, .extent = {800, 800}});
    renderer_ = std::make_unique<Renderer>(info);
    renderer_->initialize(window_);
}

void Application::run() {
    const auto start = std::chrono::steady_clock::now();
    // Cap at 60fps
    const auto period = std::chrono::duration<double>(1 / 60);

    while (!window_->should_close()) {
        window_->update();
        renderer_->update();

        const auto now = std::chrono::steady_clock::now();
        const auto iterations = (now - start) / period;
        const auto next_start = start + (iterations + 1) * period;

        std::this_thread::sleep_until(next_start);
    }
}

}  // namespace String
