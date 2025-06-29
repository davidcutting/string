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

struct WindowInfo
{
    WindowProperties properties{};
    OnKeyCallback key_callback;
//    OnMouseCallback mouse_callback;
//    OnWindowCallback window_callback;
};

class Platform;

class Window
{
    class Impl;
    using impl_t = std::experimental::propagate_const<std::unique_ptr<Impl>>;
    impl_t impl_;
public:
    ~Window();
    // Disable copy and move
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    auto get_extent() const -> Extent2D;
    bool is_open() const;

    auto get_window_properties() const -> WindowProperties;
    auto get_native_handle() const -> void*;

private:
    friend Platform;
    explicit Window(const WindowInfo& window_info);
};
}  // namespace String
