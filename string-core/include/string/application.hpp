#pragma once

#include <memory>
#include <string/core/logger.hpp>
#include <string/vulkan/renderer.hpp>
#include <string/vulkan/render_plan.hpp>
#include <string/platform/window.hpp>
#include <string/core/app_info.hpp>
#include <entt/entt.hpp>

namespace String {

/// Represents the application's state.
/// This handles the initialization of all of the application's major systems, as well as
/// running the main loop, scheduling system functionality.
class Application {
public:
    ~Application();
    /// Lazy initialization of the application's major systems. The RenderPlan declares what
    /// the renderer will draw (supplied by the application, e.g. sandbox/).
    void initialize(const ApplicationInfo& info, const RenderPlan& plan);
    /// Contains the application's main loop.
    void run();
    /// Closes the application
    void close();

private:
    /// A handle for the application's window.
    std::shared_ptr<Window> window_;
    /// A handle for the application's renderer.
    std::unique_ptr<Renderer> renderer_;
    /// Whether or not the application is/should be running.
    bool application_running_ = true;
    /// Whether or not a frame should be rendered
    bool freeze_rendering_ = false;
    /// Event handler
    entt::dispatcher event_handler_;

    void on_window_event(const WindowEvent& event);
};

}  // namespace String
