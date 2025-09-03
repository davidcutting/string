#include <string/platform/window.hpp>
#include <string/core/logger.hpp>
#include <cassert>
#include <stdexcept>


#define VK_NO_PROTOTYPES
#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>

namespace String
{

inline auto get_all_wsi_lib() -> std::vector<std::string>
{
    std::vector<std::string> wsi_libs;
    int num_drivers = SDL_GetNumVideoDrivers();
    for (int i = 0; i < num_drivers; ++i)
    {
        const auto video_driver = SDL_GetVideoDriver(i);
        wsi_libs.push_back(video_driver);
        
        STRING_LOG_DEBUG("Found video driver: {}", video_driver);
    }
    return wsi_libs;
}

inline bool attempt_sdl_init(const std::string& wsi_lib)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, wsi_lib.c_str());
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        STRING_LOG_DEBUG("Failed to initialize with {}", wsi_lib);
        return false;
    }
    STRING_LOG_DEBUG("Initialized with {}", wsi_lib);
    return true;
}

Window::Window(const Properties& properties)
: properties_(properties)
{
    SDL_SetMainReady();

    STRING_LOG_INFO("Initializing SDL...");

    bool initialized = false;
    for (const auto& wsi_lib : get_all_wsi_lib())
    {
        if (wsi_lib == "offscreen")
            throw std::runtime_error("Failed to initialize platform, String does not support offscreen");
        if (attempt_sdl_init(wsi_lib))
        {
            initialized = true;
            break;
        }
    }

    if (!initialized)
    {
        throw std::runtime_error("Failed to initialize platform - " + std::string(SDL_GetError()));
    }

    Uint32 flags = SDL_WINDOW_VULKAN;
    
    switch (properties.mode)
    {
        case String::View::Mode::WINDOWED:
            // No additional flags needed
            break;
        case String::View::Mode::FULLSCREEN_BORDERLESS:
            flags |= SDL_WINDOW_BORDERLESS;
            break;
        case String::View::Mode::FULLSCREEN:
            flags |= SDL_WINDOW_FULLSCREEN;
            break;
        // TODO(DCut): Handle headless?
        case String::View::Mode::HEADLESS:
            flags |= SDL_WINDOW_HIDDEN;
            break;
    }
    
    if (properties.resizable)
    {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    
    // Create the SDL window
    auto window_handle = SDL_CreateWindow(
        properties.title.c_str(),
        static_cast<int>(properties.extent.width),
        static_cast<int>(properties.extent.height),
        flags
    );
    
    if (window_handle == nullptr)
    {
        throw std::runtime_error("Failed to create SDL window: " + std::string(SDL_GetError()));
    }
    
    int width, height;
    SDL_GetWindowSize(window_handle, &width, &height);
    
    properties_.extent.width = static_cast<uint32_t>(width);
    properties_.extent.height = static_cast<uint32_t>(height);

    window_handle_ = (void*) window_handle;
};

Window::~Window()
{
    SDL_DestroyWindow((SDL_Window*)window_handle_);
}

void Window::update()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_EVENT_QUIT)
        {
            closing = true;
        }
        if (event.type == SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED)
        {
            int width, height;
            SDL_GetWindowSize((SDL_Window*)window_handle_, &width, &height);
            
            properties_.extent.width = static_cast<uint32_t>(width);
            properties_.extent.height = static_cast<uint32_t>(height);
        }
    }
}

void Window::register_resize_event_callback(const ResizeEventCallbackFn& fn)
{
    resize_callbacks_.push_back(fn);
}


const Window::Properties& Window::get_properties() const
{
    return properties_;
}

const View::Extent& Window::get_extent() const
{
    return properties_.extent;
}

bool Window::should_close() const
{
    return closing;
}

void* Window::get_native_handle() const
{
    assert(window_handle_ && "Window is null");
    return window_handle_;
}

VkSurfaceKHR Window::create_surface(const VkInstance& instance)
{
    VkSurfaceKHR surface;
    if (!SDL_Vulkan_CreateSurface((SDL_Window*)window_handle_, instance, nullptr, &surface))
    {
        throw std::runtime_error("failed to create window surface!");
    }
    return surface;
}

std::vector<const char*> Window::get_platform_extensions(const bool& enable_validation_layers)
{
    uint32_t extension_count = 0;
    const auto sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);

    std::vector<const char*> extensions(sdl_extensions, sdl_extensions + extension_count);

    if (enable_validation_layers)
    {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    return extensions;
}

// auto Window::is_minimized() const -> bool
// {
//     assert(window_handle_ && "Window is null");
//     Uint32 flags = SDL_GetWindowFlags(window_handle_);
//     return (flags & SDL_WINDOW_MINIMIZED) != 0;
// }

// auto Window::is_maximized() const -> bool
// {
//     assert(window_handle_ && "Window is null");
//     Uint32 flags = SDL_GetWindowFlags(window_handle_);
//     return (flags & SDL_WINDOW_MAXIMIZED) != 0;
// }

// auto Window::has_focus() const -> bool
// {
//     assert(window_handle_ && "Window is null");
//     Uint32 flags = SDL_GetWindowFlags(window_handle_);
//     return (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
// }

// auto Window::show() -> void
// {
//     assert(window_handle_ && "Window is null");
//     SDL_ShowWindow(window_handle_);
// }

// auto Window::hide() -> void
// {
//     assert(window_handle_ && "Window is null");
//     SDL_HideWindow(window_handle_);
// }

}