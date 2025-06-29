#include <string/platform/window.hpp>
#include <string/platform/glfw_window.hpp>
#include <string/logger.hpp>

#define GLFW_NO_INCLUDE
#include <GLFW/glfw3.h>

namespace String
{

// Window class implementation
Window::Window(const WindowInfo& window_info)
: impl_(std::make_unique<Impl>(window_info))
{
    // Constructor
}

Window::~Window()
{
    glfwDestroyWindow(impl_->get_glfw_window());
    glfwTerminate();
}

auto Window::get_extent() const -> Extent2D
{
    int width, height;
    glfwGetWindowSize(impl_->get_glfw_window(), &width, &height);
    return {
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
    };
}

bool Window::is_open() const
{
    return !glfwWindowShouldClose(impl_->get_glfw_window());
}

auto Window::get_window_properties() const -> WindowProperties
{
    return impl_->get_window_properties();
}

auto Window::get_native_handle() const -> void*
{
    return impl_->get_glfw_window();
}

void Window::update() {
    glfwPollEvents();
}

} // namespace String
