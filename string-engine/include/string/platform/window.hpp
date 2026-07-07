#pragma once

#include <functional>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <volk.h>

#include <string/platform/input.hpp>

namespace String
{

namespace View
{

struct Extent
{
    uint32_t width;
    uint32_t height;
};

enum class Mode { WINDOWED, FULLSCREEN, FULLSCREEN_BORDERLESS, HEADLESS };

enum class VSync { OFF, ON };

}  // namespace View

struct WindowEvent
{
    bool close = false;
    bool minimize = false;
    bool maximize = false;
};

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

    void update(entt::dispatcher& dispatcher);

    void register_resize_event_callback(const ResizeEventCallbackFn& fn);

    const Properties& get_properties() const;
    const View::Extent& get_extent() const;
    // Polled input state, refreshed by update() each frame (see Input). Stable for the window's
    // lifetime, so consumers can hold the reference.
    const Input& get_input() const { return input_; }
    bool should_close() const;
    void resize(const View::Extent& extent);
    void key_action(int key, int scancode, int action, int mods);
    void mouse_action(double xpos, double ypos);

    void* get_native_handle() const;
    VkSurfaceKHR create_surface(const VkInstance& instance);
    std::vector<const char*> get_platform_extensions(bool enable_validation_layers);

private:
    Properties properties_;
    void* window_handle_;
    std::vector<ResizeEventCallbackFn> resize_callbacks_;
    bool closing = false;
    Input input_;
};

}  // namespace String