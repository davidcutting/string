#include <GLFW/glfw3.h>

#include <cstdint>
#include <string/core/logger.hpp>
#include <string/window.hpp>

namespace String {

static void framebuffer_resize_callback(GLFWwindow* window, int width, int height) {
    auto window_ptr = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));

    window_ptr->resize({.width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height)});
}

static void glfw_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    auto window_ptr = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    window_ptr->key_action(key, scancode, action, mods);
}

// static void glfw_mouse_callback(GLFWwindow* window, double xpos, double ypos) {
//     auto window_ptr = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
//     window_ptr->mouse_action(xpos, ypos);
// }

Window::Window(const Properties& properties) : properties_(properties) {
    attempt_wayland_init();

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window_handle_ = glfwCreateWindow(properties_.extent.width, properties_.extent.height, properties_.title.c_str(),
                                      nullptr, nullptr);

    const auto window_result = get_glfw_result();
    if (window_result.failed) {
        STRING_LOG_ERROR("Failed to create a GLFW window.");
        throw std::runtime_error(window_result.reason);
    }

    glfwSetWindowUserPointer(window_handle_, this);
    glfwSetFramebufferSizeCallback(window_handle_, framebuffer_resize_callback);
    glfwSetKeyCallback(window_handle_, glfw_key_callback);
}

Window::~Window() {
    glfwDestroyWindow(window_handle_);
    glfwTerminate();
}

void Window::update() { glfwPollEvents(); }

void Window::register_resize_event_callback(const ResizeEventCallbackFn& fn) { resize_callbacks_.push_back(fn); }

const Window::Properties& Window::get_properties() const { return properties_; }

bool Window::should_close() const { return glfwWindowShouldClose(window_handle_); }

void Window::resize(const View::Extent& extent) {
    properties_.extent = extent;

    // Dispatch resize event
    for (const auto& callback : resize_callbacks_) {
        callback(extent);
    }
}

void Window::key_action(int key, int scancode, int action, int mods) {
    (void)key;
    (void)scancode;
    (void)action;
    (void)mods;
    // impl
}

void Window::mouse_action(double xpos, double ypos) {
    (void)xpos;
    (void)ypos;
    // impl
}

bool Window::attempt_wayland_init() const {
    if (glfwPlatformSupported(GLFW_PLATFORM_WAYLAND)) {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
        STRING_LOG_INFO("GLFW supports Wayland display.");
    }
    glfwInit();

    const auto wayland_init_result = get_glfw_result();
    if (wayland_init_result.failed) {
        STRING_LOG_ERROR("Failed to initialize GLFW for Wayland.");
        STRING_LOG_ERROR(wayland_init_result.reason);

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

Window::GLFWError Window::get_glfw_result() const {
    const char* result_description[100];
    if (glfwGetError(result_description) != GLFW_NO_ERROR) {
        return {.failed = true, .reason = std::string(*result_description)};
    }

    return {
        .failed = false,
        .reason = "",
    };
}

}  // namespace String
