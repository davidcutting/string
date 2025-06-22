#include <string/platform/linux/linux_window.hpp>

namespace String
{

Window::Impl::Impl(const WindowConfig& config)
: properties_(config.properties)
, window_user_data_(new detail::WindowCallbackData{
    .key_callback = config.key_callback,
    .mouse_button_callback = config.mouse_button_callback,
    .mouse_move_callback = config.mouse_move_callback,
    .mouse_scroll_callback = config.mouse_scroll_callback
  })
{
    initialize_glfw();

    // Create Window

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, config.properties.resizable ? GLFW_TRUE : GLFW_FALSE);

    GLFWmonitor* monitor = nullptr;
    if (config.properties.mode == WindowMode::FULLSCREEN) {
        monitor = glfwGetPrimaryMonitor();
    }

    window_handle = glfwCreateWindow(
        config.properties.extent.x,
        config.properties.extent.y,
        config.properties.title.c_str(),
        monitor,
        nullptr
    );

    if (!window_handle) {
        auto error = get_glfw_error();
        STRING_LOG_ERROR("Failed to create GLFW window: " + error.reason);
        throw std::runtime_error("Failed to create window: " + error.reason);
    }

    // Handle fullscreen borderless
    if (config.properties.mode == WindowMode::FULLSCREEN_BORDERLESS) {
        GLFWmonitor* primary = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(primary);
        glfwSetWindowMonitor(window_handle, primary, 0, 0, mode->width, mode->height, mode->refreshRate);
    }

    // Setup Callbacks
    setup_callbacks();
}

Window::Impl::~Impl()
{
    if (window_handle)
    {
        glfwDestroyWindow(window_handle);
    }
    glfwTerminate();
}

void Window::Impl::initialize_glfw()
{
    // Try Wayland first if supported
    if (glfwPlatformSupported(GLFW_PLATFORM_WAYLAND)) {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
        STRING_LOG_INFO("GLFW supports Wayland display.");
    }

    if (!glfwInit()) {
        auto error = get_glfw_error();
        STRING_LOG_ERROR("Failed to initialize GLFW for Wayland: " + error.reason);
        
        // Fallback to X11
        if (glfwPlatformSupported(GLFW_PLATFORM_X11)) {
            glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
            STRING_LOG_INFO("GLFW supports X11 display.");
            
            if (!glfwInit()) {
                auto x11_error = get_glfw_error();
                STRING_LOG_ERROR("Failed to initialize GLFW for X11: " + x11_error.reason);
                throw std::runtime_error("Failed to initialize GLFW on any supported platform");
            }
            STRING_LOG_INFO("Using X11 as fallback.");
        } else {
            throw std::runtime_error("No supported display platform available");
        }
    }
}

void Window::Impl::setup_callbacks()
{

    glfwSetWindowUserPointer(window_handle, window_user_data_);

    // Set GLFW callbacks
    glfwSetKeyCallback(window_handle, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
        auto* data = static_cast<detail::WindowCallbackData*>(glfwGetWindowUserPointer(window));
        if (data && data->key_callback) {
            data->key_callback(create_key_event(key, scancode, action, mods));
        }
    });

    glfwSetMouseButtonCallback(window_handle, [](GLFWwindow* window, int button, int action, int mods) {
        auto* data = static_cast<detail::WindowCallbackData*>(glfwGetWindowUserPointer(window));
        if (data && data->mouse_button_callback) {
            
            data->mouse_button_callback(create_mouse_button_event(button, action, mods));
        }
    });

    glfwSetCursorPosCallback(window_handle, [](GLFWwindow* window, double xpos, double ypos) {
        auto* data = static_cast<detail::WindowCallbackData*>(glfwGetWindowUserPointer(window));
        if (data && data->mouse_move_callback) {
            data->mouse_move_callback(xpos, ypos);
        }
    });

    glfwSetScrollCallback(window_handle, [](GLFWwindow* window, double xoffset, double yoffset) {
        auto* data = static_cast<detail::WindowCallbackData*>(glfwGetWindowUserPointer(window));
        if (data && data->mouse_scroll_callback) {
            data->mouse_scroll_callback(create_mouse_scroll_event(xoffset, yoffset));
        }
    });

    glfwSetFramebufferSizeCallback(window_handle, [](GLFWwindow* window, int width, int height) {
        // Handle framebuffer resize if needed
        // This could trigger a resize event callback if implemented
    });

    glfwSetWindowCloseCallback(window_handle, [](GLFWwindow* window) {
        // Mark window for closure but don't actually close it
        // Let the application decide when to close
    });
}

}