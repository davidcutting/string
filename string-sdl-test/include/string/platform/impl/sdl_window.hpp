#pragma once

#include <string/platform/window.hpp>
#include <SDL3/SDL.h>

namespace String::Platform
{

class Window::Impl
{
    SDL_Window* window_;
    WindowInfo info_;
public:
    explicit Impl(const WindowInfo& info);
    ~Impl();

    auto get_window_info() const -> WindowInfo;
    auto get_native_handle() const -> void*;
    auto is_minimized() const -> bool;
    auto is_maximized() const -> bool;
    auto has_focus() const -> bool;
    auto show() -> void;
    auto hide() -> void;
};

}