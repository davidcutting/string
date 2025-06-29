#pragma once

#include <string/platform/window.hpp>
#include <string/logger.hpp>
#include <string/platform/glfw_helper.hpp>

#define GLFW_NO_INCLUDE
#include <GLFW/glfw3.h>

namespace String
{

struct GLFWError
{
    bool failed;
    std::string reason;
};

class Window::Impl
{
    struct GLFWError
    {
        bool failed;
        std::string reason;
    };
    GLFWwindow* window_handle_ = nullptr;
    WindowProperties properties_;

public:
    explicit Impl(const WindowInfo& window_info);
    ~Impl();

    void initialize_glfw();
    void setup_callbacks();
    auto get_window_properties() const -> WindowProperties;
    auto get_glfw_window() const -> GLFWwindow*;

private:
    GLFWError get_glfw_error() const {
        const char* description;
        int error_code = glfwGetError(&description);
        
        if (error_code != GLFW_NO_ERROR) {
            return {true, description ? std::string(description) : "Unknown GLFW error"};
        }
        
        return {false, ""};
    }
};

}
