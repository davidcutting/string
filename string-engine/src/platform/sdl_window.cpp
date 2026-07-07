#include <string/platform/window.hpp>
#include <string/core/logger.hpp>
#include <string/core/platform_detection.hpp>
#include <cassert>
#include <stdexcept>
#include <entt/entt.hpp>

#define VK_NO_PROTOTYPES
#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>

namespace String
{

namespace
{

// Map the SDL scancodes we care about to our backend-neutral KeyCode (others are ignored).
KeyCode translate_scancode(SDL_Scancode scancode)
{
    switch (scancode)
    {
        case SDL_SCANCODE_W:      return KeyCode::W;
        case SDL_SCANCODE_A:      return KeyCode::A;
        case SDL_SCANCODE_S:      return KeyCode::S;
        case SDL_SCANCODE_D:      return KeyCode::D;
        case SDL_SCANCODE_Q:      return KeyCode::Q;
        case SDL_SCANCODE_E:      return KeyCode::E;
        case SDL_SCANCODE_SPACE:  return KeyCode::SPACE;
        case SDL_SCANCODE_LSHIFT: return KeyCode::LEFT_SHIFT;
        case SDL_SCANCODE_LCTRL:  return KeyCode::LEFT_CONTROL;
        case SDL_SCANCODE_ESCAPE: return KeyCode::ESCAPE;
        default:                  return KeyCode::UNKNOWN;
    }
}

MouseButton translate_button(Uint8 button)
{
    switch (button)
    {
        case SDL_BUTTON_RIGHT:  return MouseButton::RIGHT;
        case SDL_BUTTON_MIDDLE: return MouseButton::MIDDLE;
        default:                return MouseButton::LEFT;
    }
}

}  // namespace

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

    // Start in mouse-look (relative) mode: cursor hidden + locked, motion reported as deltas.
    // Esc releases it (see update()), a click re-captures.
    SDL_SetWindowRelativeMouseMode(window_handle, true);
    input_.set_mouse_captured(true);
};

Window::~Window()
{
    SDL_DestroyWindow((SDL_Window*)window_handle_);
}

void Window::update(entt::dispatcher& dispatcher)
{
    // Reset per-frame accumulators (the mouse delta); persistent key/button state carries over.
    input_.new_frame();

    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
            case SDL_EVENT_QUIT:
            {
                dispatcher.trigger(WindowEvent{
                    .close = true,
                });
                closing = true;
                break;
            }
            case SDL_EVENT_WINDOW_RESIZED:
            {
                int width, height;
                SDL_GetWindowSize((SDL_Window*)window_handle_, &width, &height);

                if (width <= 0 || height <= 0)
                    continue;

                properties_.extent.width = static_cast<uint32_t>(width);
                properties_.extent.height = static_cast<uint32_t>(height);
                break;
            }
            case SDL_EVENT_WINDOW_MINIMIZED:
            {
                dispatcher.trigger(WindowEvent{
                    .minimize = true,
                });
                break;
            }
            case SDL_EVENT_WINDOW_RESTORED:
            {
                dispatcher.trigger(WindowEvent{
                    .maximize = true,
                });
                break;
            }
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            {
                const bool down = event.type == SDL_EVENT_KEY_DOWN;
                const KeyCode key = translate_scancode(event.key.scancode);
                if (key != KeyCode::UNKNOWN)
                    input_.set_key(key, down);

                // Esc releases the captured cursor (frees it to interact with the desktop/UI).
                if (down && event.key.scancode == SDL_SCANCODE_ESCAPE)
                {
                    SDL_SetWindowRelativeMouseMode((SDL_Window*)window_handle_, false);
                    input_.set_mouse_captured(false);
                }
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:
            {
                // Only feed look-deltas while captured, so a released cursor doesn't turn.
                if (input_.mouse_captured())
                    input_.add_mouse_delta({ event.motion.xrel, event.motion.yrel });
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            {
                const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                input_.set_mouse_button(translate_button(event.button.button), down);

                // A click while released re-captures the cursor back into mouse-look.
                if (down && !input_.mouse_captured())
                {
                    SDL_SetWindowRelativeMouseMode((SDL_Window*)window_handle_, true);
                    input_.set_mouse_captured(true);
                }
                break;
            }
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
        throw std::runtime_error("Failed to create window surface!");
    }
    return surface;
}

std::vector<const char*> Window::get_platform_extensions(bool enable_validation_layers)
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