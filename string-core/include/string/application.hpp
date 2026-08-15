#pragma once

#include <functional>
#include <memory>
#include <string/core/logger.hpp>
#include <string/vulkan/renderer.hpp>
#include <string/vulkan/frame_graph.hpp>
#include <string/platform/window.hpp>
#include <string/core/app_info.hpp>
#include <entt/entt.hpp>

namespace string {

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

    /// Anchor an app-level service (asset registry, game state) to the application's teardown
    /// order. Type-erased on purpose: string-core cannot name string-scene types, but it IS the
    /// only place that knows the one legal destruction order — adopted services die in
    /// ~Application AFTER the scene's passes (which record against their GPU state) and BEFORE
    /// the renderer (whose allocator their destructors free through). Services survive scene
    /// switches; that is the point of adopting them here instead of capturing them in a scene.
    /// Destroyed in reverse adoption order.
    void adopt(std::shared_ptr<void> service) { adopted_.push_back(std::move(service)); }

    /// The info initialize() ran with, with the platform-derived directories filled in.
    const ApplicationInfo& info() const { return info_; }

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
    /// A window resize latched by the event callback, applied between frames in run().
    std::optional<View::Extent> pending_resize_;
    /// App-level services anchored to the teardown order (see adopt()).
    std::vector<std::shared_ptr<void>> adopted_;
    /// The resolved ApplicationInfo (empty directory fields filled from the platform).
    ApplicationInfo info_;
    /// Whether or not the application is/should be running.
    bool application_running_ = true;
    /// Whether or not a frame should be rendered
    bool freeze_rendering_ = false;
    /// Event handler
    entt::dispatcher event_handler_;

    void on_window_event(const WindowEvent& event);
    /// Tear down the running scene and author the next one into a cleared graph. Called from run()
    /// between frames — never mid-record, because it destroys the passes being recorded.
    // Takes the callback and the name rather than a SceneRegistry::Scene: scene_registry.hpp
    // includes THIS header (it stores an Application::scene_fn), so naming its types here would be
    // circular — and the loader needs nothing else from a registry entry.
    void load_scene(const scene_fn& configure, std::string_view name);
};

}  // namespace string
