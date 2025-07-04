#include <memory>
#include <string/platform/window.hpp>
#include <string/logger.hpp>

namespace String
{

struct Window::Impl
{

};

Window::Window(const WindowInfo& window_info)
: pimpl_(std::make_unique<Impl>())
, info_(window_info)
{

}

Window::~Window()
{

}

Window::Window(const Window& other)
{

}

Window& Window::operator=(const Window& other)
{

}

auto Window::get_window_info() const -> WindowInfo
{

}

auto Window::get_native_handle() const -> void*
{

}

auto Window::is_minimized() const -> bool
{

}

auto Window::is_maximized() const -> bool
{

}

auto Window::has_focus() const -> bool
{

}

auto Window::show() -> void
{

}

auto Window::hide() -> void
{

}

}