#pragma once

#include <string/platform/event.hpp>

#include <string>
#include <memory>
#include <experimental/propagate_const>

namespace String
{

struct Extent2D
{
    uint32_t x;
    uint32_t y;
};

enum class WindowMode
{
    WINDOWED,
    FULLSCREEN,
    FULLSCREEN_BORDERLESS,
    HEADLESS
};

struct WindowProperties
{
    std::string title = "String Engine";
    WindowMode mode = WindowMode::WINDOWED;
    Extent2D extent = {1280, 720};
    bool resizable = true;
    bool vsync = true;
};

struct WindowConfig
{
    WindowProperties properties{};
    OnKeyCallback key_callback;
    OnMouseButtonCallback mouse_button_callback;
    OnMouseMoveCallback mouse_move_callback;
    OnMouseScrollCallback mouse_scroll_callback;
};

class Platform;

class Window
{
    class Impl;
    std::experimental::propagate_const<std::unique_ptr<Impl>> impl_;
public:
    explicit Window(const WindowConfig& window_config);
    ~Window();

    // Disable copy and move
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    void update();

    void set_extent(const Extent2D& extent);
    auto get_extent() const -> Extent2D;
    bool should_close() const;
    void set_should_close(bool should_close);
    auto get_title() const -> std::string;
    void set_title(const std::string& title);
    bool is_minimized() const;
    bool is_maximized() const;
    bool is_focused() const;
    bool is_visible() const;
    void show();
    void hide();
    void minimize();
    void maximize();
    void restore();
    void focus();
    bool is_fullscreen() const;
    void set_fullscreen(const bool& fullscreen = true);

    const WindowProperties& get_window_properties() const;
    void* get_native_handle() const;

private:
    friend Platform;
};
}  // namespace String