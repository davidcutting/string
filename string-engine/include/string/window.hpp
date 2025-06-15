#pragma once

#include <functional>
#include <string>
#include <vector>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace String {
namespace View {
struct Extent {
    uint32_t width;
    uint32_t height;
};

enum class Mode { WINDOWED, FULLSCREEN, FULLSCREEN_BORDERLESS, HEADLESS };

enum class VSync { OFF, ON };
}  // namespace View

class Window {
public:
    using ResizeEventCallbackFn = std::function<void(View::Extent const&)>;

    struct Properties {
        std::string title = "unnamed window";
        View::Mode mode = View::Mode::WINDOWED;
        bool resizable = true;
        View::VSync vsync = View::VSync::ON;
        View::Extent extent = {1280, 720};
    };

    Window(const Properties& properties);
    ~Window();

    void update();

    void register_resize_event_callback(const ResizeEventCallbackFn& fn);

    const Properties& get_properties() const;
    bool should_close() const;
    void resize(const View::Extent& extent);
    void key_action(int key, int scancode, int action, int mods);
    void mouse_action(double xpos, double ypos);

    GLFWwindow* get_native_handle() const { return window_handle_; };

private:
    Properties properties_;
    GLFWwindow* window_handle_;
    std::vector<ResizeEventCallbackFn> resize_callbacks_;

    struct GLFWError {
        bool failed{false};
        std::string reason{""};
    };

    GLFWError get_glfw_result() const;

    bool attempt_wayland_init() const;
};
}  // namespace String
