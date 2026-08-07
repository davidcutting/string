#pragma once

#include <functional>
#include <memory>
#include <string/core/logger.hpp>
#include <string/vulkan/renderer.hpp>
#include <string/vulkan/frame_graph.hpp>
#include <string/platform/window.hpp>
#include <string/core/app_info.hpp>
#include <entt/entt.hpp>

namespace String {

/// Represents the application's state.
/// This handles the initialization of all of the application's major systems, as well as
/// running the main loop, scheduling system functionality.
///
/// Brief 20: the application owns its render graph. It is handed a `scene` — one callback that
/// constructs the app's pass objects, declares them onto the graph, and returns the per-frame tick —
/// rather than a RenderPlan the renderer drives. What the app declares is the app's; what is derived
/// from those declarations (acquire, resize, queue placement, present) stays behind render_frame.
class Application {
public:
    /// Constructs the scene against the ready graph and engine context, and returns the per-frame
    /// CPU tick to run before each execute. Called once, after the renderer is up.
    // Takes the graph it authors into, the build-time services, and the renderer — the last so the
    // scene can declare which of ITS images the headless capture gates read (the renderer cannot
    // know: the HDR target is an app-declared transient).
    using scene_fn = std::function<std::function<void(float)>(
        string::frame_graph&, engine_context&, string::renderer&)>;

    ~Application();
    /// Lazy initialization of the application's major systems.
    void initialize(const ApplicationInfo& info, scene_fn scene);
    /// Contains the application's main loop.
    void run();
    /// Closes the application
    void close();

private:
    /// A handle for the application's window.
    std::shared_ptr<Window> window_;
    /// A handle for the application's renderer.
    std::unique_ptr<string::renderer> renderer_;
    /// The application's render graph, and the compiled plan executed each frame. Authored ONCE.
    string::frame_graph graph_;
    string::compiled_frame frame_;
    /// Per-frame CPU work the scene returned from its construction callback.
    std::function<void(float)> tick_;
    /// Whether or not the application is/should be running.
    bool application_running_ = true;
    /// Whether or not a frame should be rendered
    bool freeze_rendering_ = false;
    /// Event handler
    entt::dispatcher event_handler_;

    void on_window_event(const WindowEvent& event);
};

}  // namespace String
