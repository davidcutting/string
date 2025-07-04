#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <string/platform/impl/glfw_platform.hpp>
#include <string/platform/impl/glfw_window.hpp>
#include <string/logger.hpp>

namespace String::Platform
{

Platform::Impl::Impl()
{
    std::println("Initializing GLFW...");

    if (!attempt_wayland_init())
    {
        throw std::runtime_error("Failed to initialize platform!");
    }

    std::println("GLFW initialized successfully");
}

Platform::Impl::~Impl()
{
    std::println("Shutting down GLFW...");
    glfwTerminate();
}

bool Platform::Impl::attempt_wayland_init() const {
    if (glfwPlatformSupported(GLFW_PLATFORM_WAYLAND)) {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
        STRING_LOG_INFO("GLFW supports Wayland display.");
    }
    glfwInit();

    const auto wayland_init_result = get_glfw_result();
    if (wayland_init_result.failed) {
        STRING_LOG_ERROR("Failed to initialize GLFW for Wayland.");
        STRING_LOG_ERROR("{}", wayland_init_result.reason);

        // Fallback to X11
        if (glfwPlatformSupported(GLFW_PLATFORM_X11)) {
            glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
            STRING_LOG_INFO("GLFW supports X11 display.");
        }
        glfwInit();

        const auto x11_init_result = get_glfw_result();
        if (x11_init_result.failed) {
            STRING_LOG_ERROR("Failed to create a X11 window.");
            throw std::runtime_error(x11_init_result.reason);
        }

        STRING_LOG_INFO("Window created using X11 as fallback.");
        return false;
    }
    return true;
}

bool Platform::Impl::poll_events()
{
    glfwPollEvents();
    return glfwWindowShouldClose(window);
}

}