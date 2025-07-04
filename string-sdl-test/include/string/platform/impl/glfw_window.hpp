#pragma once

#include <string/platform/window.hpp>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace String::Platform
{

struct GLFWError
{
    bool failed{false};
    std::string reason{""};
};

class Window::Impl
{
    GLFWwindow* window_;
    WindowInfo info_;
public:
    explicit Impl(const WindowInfo& info);
    ~Impl();

    auto get_window_info() const -> WindowInfo;
    auto get_native_handle() const -> void*;
    auto is_minimized() const -> bool;
    auto is_maximized() const -> bool;
    auto has_focus() const -> bool;
    auto show() -> void;
    auto hide() -> void;
};

inline auto get_glfw_result() -> GLFWError
{
    const char* result_description[100];
    if (glfwGetError(result_description) != GLFW_NO_ERROR) {
        return {.failed = true, .reason = std::string(*result_description)};
    }

    return {
        .failed = false,
        .reason = "",
    };
}

}