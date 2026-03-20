#pragma once

#include <memory>
#include <string/core/logger.hpp>
#include <string/vulkan/renderer.hpp>
#include <string/platform/window.hpp>
#include <string/core/app_info.hpp>
#include <entt/entt.hpp>
#include <string/scene.hpp>

namespace String {

/// Represents the application's state.
/// This handles the initialization of all of the application's major systems, as well as
/// running the main loop, scheduling system functionality.
class Application {
public:
    ~Application();
    /// Lazy initialization of the application's major systems.
    void initialize(const ApplicationInfo& info);
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
    /// The scene
    Scene scene_;

    void on_window_event(const WindowEvent& event);
};

}  // namespace String
