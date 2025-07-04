#pragma once

#include <memory>
#include <string>
#include <string/math.hpp>

namespace String
{

enum class WindowMode
{
    Windowed,
    Borderless,
    Fullscreen
};

struct WindowInfo
{
    std::string title = "String Engine";
    Extent2D extent = {1280, 720};
    WindowMode mode = WindowMode::Windowed;
    bool resizable = true;
};

class Window
{
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
    WindowInfo info_;
public:
    explicit Window(const WindowInfo& window_info);
    ~Window();

    Window(const Window& other);
    Window& operator=(const Window& other);
    Window(Window&& other) noexcept = default;
    Window& operator=(Window&& other) noexcept = default;

    auto get_window_info() const -> WindowInfo;
    auto get_native_handle() const -> void*;
    auto is_minimized() const -> bool;
    auto is_maximized() const -> bool;
    auto has_focus() const -> bool;
    auto show() -> void;
    auto hide() -> void;
private:
    friend class Platform;
};
}
