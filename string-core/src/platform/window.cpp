#include <string/platform/window.hpp>
#include <string/logger.hpp>

#include <GLFW/glfw3.h>
#include <stdexcept>
#include <iostream>

namespace String
{

KeyCode glfw_key_to_keycode(int glfw_key);
MouseButton glfw_button_to_mouse_button(int glfw_button);
KeyAction glfw_action_to_key_action(int glfw_action);
MouseAction glfw_action_to_mouse_action(int glfw_action);
KeyModifier glfw_mods_to_key_modifier(int glfw_mods);

KeyEvent create_key_event(int key, int scancode, int action, int mods);
MouseButtonEvent create_mouse_button_event(int button, int action, int mods, double x, double y);
MouseScrollEvent create_mouse_scroll_event(double xoffset, double yoffset, double x, double y, int mods);

class Window::Impl
{
public:
    GLFWwindow* window_handle = nullptr;
    bool should_close_flag = false;

    struct GLFWError {
        bool failed;
        std::string reason;
    };

    explicit Impl(const WindowConfig& config) {
        initialize_glfw();
        create_window(config);
        setup_callbacks(config);
    }

    ~Impl() {
        if (window_handle) {
            glfwDestroyWindow(window_handle);
        }
        glfwTerminate();
    }

    void initialize_glfw() {
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

    void create_window(const WindowConfig& config) {
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

        // Set VSync
        if (config.properties.vsync) {
            // Note: VSync is typically handled by the graphics API (Vulkan/OpenGL)
            // This is just a placeholder for the property
        }
    }

    void setup_callbacks(const WindowConfig& config) {
        // Store callbacks in the window user pointer structure
        auto* window_data = new WindowCallbackData{
            config.key_callback,
            config.mouse_button_callback,
            config.mouse_move_callback,
            config.mouse_scroll_callback
        };

        glfwSetWindowUserPointer(window_handle, window_data);

        // Set GLFW callbacks
        glfwSetKeyCallback(window_handle, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
            auto* data = static_cast<WindowCallbackData*>(glfwGetWindowUserPointer(window));
            if (data && data->key_callback) {
                data->key_callback(key, scancode, action, mods);
            }
        });

        glfwSetMouseButtonCallback(window_handle, [](GLFWwindow* window, int button, int action, int mods) {
            auto* data = static_cast<WindowCallbackData*>(glfwGetWindowUserPointer(window));
            if (data && data->mouse_button_callback) {
                data->mouse_button_callback(button, action, mods);
            }
        });

        glfwSetCursorPosCallback(window_handle, [](GLFWwindow* window, double xpos, double ypos) {
            auto* data = static_cast<WindowCallbackData*>(glfwGetWindowUserPointer(window));
            if (data && data->mouse_move_callback) {
                data->mouse_move_callback(xpos, ypos);
            }
        });

        glfwSetScrollCallback(window_handle, [](GLFWwindow* window, double xoffset, double yoffset) {
            auto* data = static_cast<WindowCallbackData*>(glfwGetWindowUserPointer(window));
            if (data && data->mouse_scroll_callback) {
                data->mouse_scroll_callback(xoffset, yoffset);
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

    GLFWError get_glfw_error() const {
        const char* description;
        int error_code = glfwGetError(&description);
        
        if (error_code != GLFW_NO_ERROR) {
            return {true, description ? std::string(description) : "Unknown GLFW error"};
        }
        
        return {false, ""};
    }

private:
    struct WindowCallbackData {
        OnKeyCallback key_callback;
        OnMouseButtonCallback mouse_button_callback;
        OnMouseMoveCallback mouse_move_callback;
        OnMouseScrollCallback mouse_scroll_callback;
    };
};

// Window class implementation
Window::Window(const WindowConfig& window_config)
: impl_(std::make_unique<Impl>(window_config))
, properties_(window_config.properties)
, key_callback_(window_config.key_callback)
, mouse_button_callback_(window_config.mouse_button_callback)
, mouse_move_callback_(window_config.mouse_move_callback)
, mouse_scroll_callback_(window_config.mouse_scroll_callback)
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