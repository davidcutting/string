#pragma once

#include <string/platform/window.hpp>
#include <string/logger.hpp>
#include <string/platform/linux/glfw_helper.hpp>

#include <GLFW/glfw3.h>

namespace String
{
namespace detail
{

struct WindowCallbackData
{
    OnKeyCallback key_callback;
    OnMouseButtonCallback mouse_button_callback;
    OnMouseMoveCallback mouse_move_callback;
    OnMouseScrollCallback mouse_scroll_callback;
};

struct GLFWError
{
    bool failed;
    std::string reason;
};

}

class Window::Impl
{
public:
    WindowProperties properties_;
    OnKeyCallback key_callback_;
    OnMouseButtonCallback mouse_button_callback_;
    OnMouseMoveCallback mouse_move_callback_;
    OnMouseScrollCallback mouse_scroll_callback_;

    explicit Impl(const WindowConfig& config);
    ~Impl();

    void initialize_glfw();
    void setup_callbacks();

private:
    GLFWwindow* window_handle = nullptr;
    detail::WindowCallbackData* window_user_data_ = nullptr;
    bool should_close_flag = false;

    detail::GLFWError get_glfw_error() const {
        const char* description;
        int error_code = glfwGetError(&description);
        
        if (error_code != GLFW_NO_ERROR) {
            return {true, description ? std::string(description) : "Unknown GLFW error"};
        }
        
        return {false, ""};
    }
    
};

}