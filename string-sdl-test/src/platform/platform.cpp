#include <memory>
#include <string/platform/platform.hpp>
#include <print>

#ifdef STRING_SDL
#include <string/platform/impl/sdl_platform.hpp>
#else
#include <string/platform/impl/glfw_platform.hpp>
#endif

namespace String::Platform
{

auto to_string(PlatformError error) -> std::string
{
    switch (error) {
        case PlatformError::WindowInitFailed:
            return "Failed to initialize window";
        case PlatformError::DeviceInitFailed:
            return "Failed to create device";
        default:
            return "Unknown platform error";
    }
}

Platform::Platform()
: pimpl_(std::make_unique<Impl>())
{
}

Platform::~Platform()
{
    // no-op
}

auto Platform::create_window(const WindowInfo& info) -> std::expected<std::unique_ptr<Window>, PlatformError>
{
    try {
        auto window = std::make_unique<Window>(info);
        pimpl_->window = static_cast<GLFWwindow*>(window->get_native_handle());
        return std::move(window);
    } catch (const std::exception& e) {
        std::println("Window creation failed: {}", e.what());
        return std::unexpected(PlatformError::WindowInitFailed);
    }
}

bool Platform::poll_events()
{
    return pimpl_->poll_events();
}

}