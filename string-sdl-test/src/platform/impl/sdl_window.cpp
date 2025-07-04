#include <string/platform/impl/sdl_window.hpp>
#include <cassert>
#include <stdexcept>

namespace String::Platform
{

Window::Impl::Impl(const WindowInfo& window_info)
: info_(window_info)
{
    // Convert WindowMode to SDL flags
    // Uint32 flags = SDL_WINDOW_VULKAN; // Assuming Vulkan usage
    Uint32 flags = SDL_WINDOW_VULKAN; // No Vulkan assumption :o
    
    switch (window_info.mode)
    {
        case WindowMode::Windowed:
            // No additional flags needed
            break;
        case WindowMode::Borderless:
            flags |= SDL_WINDOW_BORDERLESS;
            break;
        case WindowMode::Fullscreen:
            flags |= SDL_WINDOW_FULLSCREEN;
            break;
    }
    
    if (window_info.resizable)
    {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    
    // Create the SDL window
    window_ = SDL_CreateWindow(
        window_info.title.c_str(),
        static_cast<int>(window_info.extent.width),
        static_cast<int>(window_info.extent.height),
        flags
    );
    
    if (window_ == NULL)
    {
        throw std::runtime_error("Failed to create SDL window: " + std::string(SDL_GetError()));
    }
    
    int width, height;
    SDL_GetWindowSize(window_, &width, &height);
    
    info_.extent.width = static_cast<uint32_t>(width);
    info_.extent.height = static_cast<uint32_t>(height);
};

Window::Impl::~Impl()
{
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
}

auto Window::Impl::get_window_info() const -> WindowInfo
{
    return info_;
}

auto Window::Impl::get_native_handle() const -> void*
{
    assert(window_ && "Window is null");
    
    // Get the native window handle for Vulkan surface creation
    SDL_PropertiesID props = SDL_GetWindowProperties(window_);
    
#if defined(SDL_PLATFORM_WIN32)
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(SDL_PLATFORM_MACOS)
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#elif defined(SDL_PLATFORM_LINUX)
    // For X11
    if (SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr)) {
        return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    }
    // For Wayland
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
#else
    return nullptr;
#endif
}

auto Window::Impl::is_minimized() const -> bool
{
    assert(window_ && "Window is null");
    Uint32 flags = SDL_GetWindowFlags(window_);
    return (flags & SDL_WINDOW_MINIMIZED) != 0;
}

auto Window::Impl::is_maximized() const -> bool
{
    assert(window_ && "Window is null");
    Uint32 flags = SDL_GetWindowFlags(window_);
    return (flags & SDL_WINDOW_MAXIMIZED) != 0;
}

auto Window::Impl::has_focus() const -> bool
{
    assert(window_ && "Window is null");
    Uint32 flags = SDL_GetWindowFlags(window_);
    return (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
}

auto Window::Impl::show() -> void
{
    assert(window_ && "Window is null");
    SDL_ShowWindow(window_);
}

auto Window::Impl::hide() -> void
{
    assert(window_ && "Window is null");
    SDL_HideWindow(window_);
}

}