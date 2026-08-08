#define VK_NO_PROTOTYPES
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <stdexcept>
#include <cstdint>
#include <string/core/logger.hpp>
#include <string/platform/window.hpp>

namespace string
{

struct GLFWError {
    bool failed{false};
    std::string reason{""};
};

GLFWError get_glfw_result()
{
    const char* result_description[100];
    if (glfwGetError(result_description) != GLFW_NO_ERROR)
    {
        return {.failed = true, .reason = std::string(*result_description)};
    }

    return {
        .failed = false,
        .reason = "",
    };
}

bool attempt_wayland_init()
{
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

// void setup_callbacks()
// {

//     glfwSetWindowUserPointer(window_handle, window_user_data_);

//     // Set GLFW callbacks
//     glfwSetKeyCallback(window_handle, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
//         auto* data = static_cast<detail::WindowCallbackData*>(glfwGetWindowUserPointer(window));
//         if (data && data->key_callback) {
//             data->key_callback(create_key_event(key, scancode, action, mods));
//         }
//     });

//     glfwSetMouseButtonCallback(window_handle, [](GLFWwindow* window, int button, int action, int mods) {
//         auto* data = static_cast<detail::WindowCallbackData*>(glfwGetWindowUserPointer(window));
//         if (data && data->mouse_button_callback) {
            
//             data->mouse_button_callback(create_mouse_button_event(button, action, mods));
//         }
//     });

//     glfwSetCursorPosCallback(window_handle, [](GLFWwindow* window, double xpos, double ypos) {
//         auto* data = static_cast<detail::WindowCallbackData*>(glfwGetWindowUserPointer(window));
//         if (data && data->mouse_move_callback) {
//             data->mouse_move_callback(xpos, ypos);
//         }
//     });

//     glfwSetScrollCallback(window_handle, [](GLFWwindow* window, double xoffset, double yoffset) {
//         auto* data = static_cast<detail::WindowCallbackData*>(glfwGetWindowUserPointer(window));
//         if (data && data->mouse_scroll_callback) {
//             data->mouse_scroll_callback(create_mouse_scroll_event(xoffset, yoffset));
//         }
//     });

//     glfwSetFramebufferSizeCallback(window_handle, [](GLFWwindow* window, int width, int height) {
//         // Handle framebuffer resize if needed
//         // This could trigger a resize event callback if implemented
//     });

//     glfwSetWindowCloseCallback(window_handle, [](GLFWwindow* window) {
//         // Mark window for closure but don't actually close it
//         // Let the application decide when to close
//     });
// }

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
    glfwWindowHint(GLFW_RESIZABLE, properties.resizable ? GLFW_TRUE : GLFW_FALSE);

    GLFWmonitor* monitor = nullptr;
    if (properties.mode == View::Mode::FULLSCREEN)
    {
        monitor = glfwGetPrimaryMonitor();
    }

    window_handle_ = glfwCreateWindow(
        properties.extent.width,
        properties.extent.height,
        properties.title.c_str(),
        monitor,
        nullptr
    );

    if (!window_handle_)
    {
        auto error = get_glfw_result();
        STRING_LOG_ERROR("Failed to create GLFW window: {}", error.reason);
        throw std::runtime_error("Failed to create window: " + error.reason);
    }

    // Handle fullscreen borderless
    if (properties.mode == View::Mode::FULLSCREEN_BORDERLESS)
    {
        GLFWmonitor* primary = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(primary);
        glfwSetWindowMonitor((GLFWwindow*)window_handle_, primary, 0, 0, mode->width, mode->height, mode->refreshRate);
    }

    glfwSetWindowUserPointer((GLFWwindow*)window_handle_, this);
    glfwSetFramebufferSizeCallback((GLFWwindow*)window_handle_, framebuffer_resize_callback);
    glfwSetKeyCallback((GLFWwindow*)window_handle_, glfw_key_callback);
}

Window::~Window()
{
    if (window_handle_)
    {
        glfwDestroyWindow((GLFWwindow*)window_handle_);
    }
    glfwTerminate();
}

void Window::update()
{
    glfwPollEvents();
}

void Window::register_resize_event_callback(const ResizeEventCallbackFn& fn) { resize_callbacks_.push_back(fn); }

const Window::Properties& Window::get_properties() const { return properties_; }

const View::Extent& Window::get_extent() const
{
    // glfwGetFramebufferSize(window_handle_, &properties_.extent.width, &properties_.extent.height);

    // while (properties_.extent.width == 0 || properties_.extent.height == 0)
    // {
    //     glfwGetFramebufferSize(window_handle_, &properties_.extent.width, &properties_.extent.height);
    //     glfwWaitEvents();
    // }

    return properties_.extent;
}

bool Window::should_close() const
{
    return glfwWindowShouldClose((GLFWwindow*)window_handle_);
}

void Window::resize(const View::Extent& extent)
{
    properties_.extent = extent;

    // Dispatch resize event
    for (const auto& callback : resize_callbacks_)
    {
        callback(extent);
    }
}

void Window::maximize()
{
    glfwMaximizeWindow((GLFWwindow*)window_handle_);
}

void Window::set_size(const View::Extent& extent)
{
    glfwSetWindowSize((GLFWwindow*)window_handle_, static_cast<int>(extent.width),
                      static_cast<int>(extent.height));
}

void Window::key_action(int key, int scancode, int action, int mods)
{
    (void)key;
    (void)scancode;
    (void)action;
    (void)mods;
    // impl
}

void Window::mouse_action(double xpos, double ypos)
{
    (void)xpos;
    (void)ypos;
    // impl
}

VkSurfaceKHR Window::create_surface(const VkInstance& instance)
{
    VkSurfaceKHR surface;
    if (glfwCreateWindowSurface(instance, (GLFWwindow*)window_handle_, nullptr, &surface) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create window surface!");
    }
    return surface;
}

std::vector<const char*> Window::get_platform_extensions(bool enable_validation_layers)
{
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

    if (enable_validation_layers)
    {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    return extensions;
}

}  // namespace string
