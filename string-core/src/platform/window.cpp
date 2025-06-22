#include <string/platform/window.hpp>
#include <string/logger.hpp>

#include <GLFW/glfw3.h>
#include <stdexcept>
#include <iostream>

namespace String
{

// Window class implementation
Window::Window(const WindowConfig& window_config)
: impl_(std::make_unique<Impl>(window_config))

{
    // Constructor
}

Window::~Window() = default;

void Window::update() {
    glfwPollEvents();
}

void Window::set_extent(const Extent2D& extent) {
    properties_.extent = extent;
    glfwSetWindowSize(impl_->window_handle, extent.x, extent.y);
}

auto Window::get_extent() const -> Extent2D {
    int width, height;
    glfwGetWindowSize(impl_->window_handle, &width, &height);
    return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
}

bool Window::should_close() const {
    return glfwWindowShouldClose(impl_->window_handle) || impl_->should_close_flag;
}

void Window::set_should_close(bool should_close) {
    impl_->should_close_flag = should_close;
    glfwSetWindowShouldClose(impl_->window_handle, should_close ? GLFW_TRUE : GLFW_FALSE);
}

auto Window::get_title() const -> std::string {
    return properties_.title;
}

void Window::set_title(const std::string& title) {
    properties_.title = title;
    glfwSetWindowTitle(impl_->window_handle, title.c_str());
}

bool Window::is_minimized() const {
    return glfwGetWindowAttrib(impl_->window_handle, GLFW_ICONIFIED) == GLFW_TRUE;
}

bool Window::is_maximized() const {
    return glfwGetWindowAttrib(impl_->window_handle, GLFW_MAXIMIZED) == GLFW_TRUE;
}

bool Window::is_focused() const {
    return glfwGetWindowAttrib(impl_->window_handle, GLFW_FOCUSED) == GLFW_TRUE;
}

bool Window::is_visible() const {
    return glfwGetWindowAttrib(impl_->window_handle, GLFW_VISIBLE) == GLFW_TRUE;
}

void Window::show() {
    glfwShowWindow(impl_->window_handle);
}

void Window::hide() {
    glfwHideWindow(impl_->window_handle);
}

void Window::minimize() {
    glfwIconifyWindow(impl_->window_handle);
}

void Window::maximize() {
    glfwMaximizeWindow(impl_->window_handle);
}

void Window::restore() {
    glfwRestoreWindow(impl_->window_handle);
}

void Window::focus() {
    glfwFocusWindow(impl_->window_handle);
}

bool Window::is_fullscreen() const {
    return glfwGetWindowMonitor(impl_->window_handle) != nullptr;
}

void Window::set_fullscreen(const bool& fullscreen) {
    if (fullscreen && !is_fullscreen()) {
        // Store windowed mode properties
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(impl_->window_handle, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        properties_.mode = WindowMode::FULLSCREEN;
    } else if (!fullscreen && is_fullscreen()) {
        // Restore windowed mode
        glfwSetWindowMonitor(impl_->window_handle, nullptr, 100, 100, properties_.extent.x, properties_.extent.y, 0);
        properties_.mode = WindowMode::WINDOWED;
    }
}

const WindowProperties& Window::get_window_properties() const {
    return properties_;
}

void* Window::get_native_handle() const {
    return impl_->window_handle;
}

} // namespace String