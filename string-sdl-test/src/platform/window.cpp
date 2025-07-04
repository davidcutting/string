#include <string/platform/window.hpp>

#ifdef STRING_SDL
#include <string/platform/impl/sdl_window.hpp>
#else
#include <string/platform/impl/glfw_window.hpp>
#endif

namespace String::Platform
{

Window::Window(const WindowInfo& window_info)
: pimpl_(std::make_unique<Impl>(window_info))
, info_(window_info)
{
};

Window::~Window()
{
    // no-op
}

Window::Window(const Window& other)
: pimpl_(std::make_unique<Impl>(*other.pimpl_)) {}

Window& Window::operator=(const Window& other)
{
    if (this != &other) {
        *pimpl_ = *other.pimpl_;
    }
    return *this;
}

auto Window::get_window_info() const -> WindowInfo
{
    return pimpl_->get_window_info();
}

auto Window::get_native_handle() const -> void*
{
    return pimpl_->get_native_handle();
}

auto Window::is_minimized() const -> bool
{
    return pimpl_->is_maximized();
}

auto Window::is_maximized() const -> bool
{
    return pimpl_->is_maximized();
}

auto Window::has_focus() const -> bool
{
    return pimpl_->has_focus();
}

auto Window::show() -> void
{
    pimpl_->show();
}

auto Window::hide() -> void
{
    pimpl_->hide();
}

}