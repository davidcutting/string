#include <memory>
#include <string/platform/window.hpp>
#include <cassert>
#include <stdexcept>
#include <SDL3/SDL.h>

namespace String
{

struct Window::Impl
{
    SDL_Window* window;
};

Window::Window(const WindowInfo& window_info)
: pimpl_(std::make_unique<Impl>())
, info_(window_info)
{
    Uint32 flags = SDL_WINDOW_VULKAN;
    
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
    pimpl_->window = SDL_CreateWindow(
        window_info.title.c_str(),
        static_cast<int>(window_info.extent.width),
        static_cast<int>(window_info.extent.height),
        flags
    );
    
    if (pimpl_->window == NULL)
    {
        throw std::runtime_error("Failed to create SDL window: " + std::string(SDL_GetError()));
    }
    
    int width, height;
    SDL_GetWindowSize(pimpl_->window, &width, &height);
    
    info_.extent.width = static_cast<uint32_t>(width);
    info_.extent.height = static_cast<uint32_t>(height);
};

Window::~Window()
{
    if (pimpl_->window) {
        SDL_DestroyWindow(pimpl_->window);
        pimpl_->window = nullptr;
    }
}

auto Window::get_window_info() const -> WindowInfo
{
    return info_;
}

auto Window::get_native_handle() const -> void*
{
    assert(pimpl_->window && "Window is null");
    
    // Get the native window handle for Vulkan surface creation
    SDL_PropertiesID props = SDL_GetWindowProperties(pimpl_->window);
    
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

auto Window::is_minimized() const -> bool
{
    assert(pimpl_->window && "Window is null");
    Uint32 flags = SDL_GetWindowFlags(pimpl_->window);
    return (flags & SDL_WINDOW_MINIMIZED) != 0;
}

auto Window::is_maximized() const -> bool
{
    assert(pimpl_->window && "Window is null");
    Uint32 flags = SDL_GetWindowFlags(pimpl_->window);
    return (flags & SDL_WINDOW_MAXIMIZED) != 0;
}

auto Window::has_focus() const -> bool
{
    assert(pimpl_->window && "Window is null");
    Uint32 flags = SDL_GetWindowFlags(pimpl_->window);
    return (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
}

auto Window::show() -> void
{
    assert(pimpl_->window && "Window is null");
    SDL_ShowWindow(pimpl_->window);
}

auto Window::hide() -> void
{
    assert(pimpl_->window && "Window is null");
    SDL_HideWindow(pimpl_->window);
}

}