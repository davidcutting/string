#pragma once

#include <memory>
#include <string/core/logger.hpp>
#include <string/vulkan/renderer.hpp>
#include <string/platform/window.hpp>
#include <string/core/app_info.hpp>

namespace String {

/// Represents the application's state.
/// This handles the initialization of all of the application's major systems, as well as
/// running the main loop, scheduling system functionality.
class Application {
public:
    /// Lazy initialization of the application's major systems.
    void initialize(const ApplicationInfo& info);
    /// Contains the application's main loop.
    void run();

private:
    /// A handle for the application's window.
    std::shared_ptr<Window> window_;
    /// A handle for the application's renderer.
    std::unique_ptr<Renderer> renderer_;
};

}  // namespace String
