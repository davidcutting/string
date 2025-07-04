#include <string/platform/impl/glfw_window.hpp>

namespace String::Platform
{

Window::Impl::Impl(const WindowInfo& info)
: info_(info)
{
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window_ = glfwCreateWindow(info.extent.width, info.extent.height, info.title.c_str(),
                                      nullptr, nullptr);

    const auto window_result = get_glfw_result();
    if (window_result.failed)
    {
        throw std::runtime_error(window_result.reason);
    }

    // glfwSetWindowUserPointer(window_, this);
    // glfwSetFramebufferSizeCallback(window_handle_, framebuffer_resize_callback);
    // glfwSetKeyCallback(window_handle_, glfw_key_callback);
}

Window::Impl::~Impl()
{
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    // Note: Don't call glfwTerminate() here as other windows might still exist
}

auto Window::Impl::get_window_info() const -> WindowInfo
{
    return info_;
}

auto Window::Impl::get_native_handle() const -> void*
{
    if (!window_) {
        return nullptr;
    }
    
#ifdef GLFW_EXPOSE_NATIVE_WIN32
    return glfwGetWin32Window(window_);
#elif defined(GLFW_EXPOSE_NATIVE_X11)
    return reinterpret_cast<void*>(glfwGetX11Window(window_));
#elif defined(GLFW_EXPOSE_NATIVE_WAYLAND)
    return glfwGetWaylandWindow(window_);
#elif defined(GLFW_EXPOSE_NATIVE_COCOA)
    return glfwGetCocoaWindow(window_);
#else
    // Return the GLFWwindow* as a fallback
    return static_cast<void*>(window_);
#endif
}

auto Window::Impl::is_minimized() const -> bool
{
    if (!window_) {
        return false;
    }
    
    return glfwGetWindowAttrib(window_, GLFW_ICONIFIED) == GLFW_TRUE;
}

auto Window::Impl::is_maximized() const -> bool
{
    if (!window_) {
        return false;
    }
    
    return glfwGetWindowAttrib(window_, GLFW_MAXIMIZED) == GLFW_TRUE;
}

auto Window::Impl::has_focus() const -> bool
{
    if (!window_) {
        return false;
    }
    
    return glfwGetWindowAttrib(window_, GLFW_FOCUSED) == GLFW_TRUE;
}

auto Window::Impl::show() -> void
{
    if (window_) {
        glfwShowWindow(window_);
        // info_.visible = true;
    }
}

auto Window::Impl::hide() -> void
{
    if (window_) {
        glfwHideWindow(window_);
        // info_.visible = false;
    }
}

}