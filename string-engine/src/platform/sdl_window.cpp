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

// Map an SDL scancode to our backend-neutral KeyCode. Full coverage so the action-mapping layer
// (InputMap) can bind any key; unmapped/exotic scancodes fall through to UNKNOWN.
KeyCode translate_scancode(SDL_Scancode scancode)
{
    switch (scancode)
    {
        // Letters
        case SDL_SCANCODE_A: return KeyCode::A;
        case SDL_SCANCODE_B: return KeyCode::B;
        case SDL_SCANCODE_C: return KeyCode::C;
        case SDL_SCANCODE_D: return KeyCode::D;
        case SDL_SCANCODE_E: return KeyCode::E;
        case SDL_SCANCODE_F: return KeyCode::F;
        case SDL_SCANCODE_G: return KeyCode::G;
        case SDL_SCANCODE_H: return KeyCode::H;
        case SDL_SCANCODE_I: return KeyCode::I;
        case SDL_SCANCODE_J: return KeyCode::J;
        case SDL_SCANCODE_K: return KeyCode::K;
        case SDL_SCANCODE_L: return KeyCode::L;
        case SDL_SCANCODE_M: return KeyCode::M;
        case SDL_SCANCODE_N: return KeyCode::N;
        case SDL_SCANCODE_O: return KeyCode::O;
        case SDL_SCANCODE_P: return KeyCode::P;
        case SDL_SCANCODE_Q: return KeyCode::Q;
        case SDL_SCANCODE_R: return KeyCode::R;
        case SDL_SCANCODE_S: return KeyCode::S;
        case SDL_SCANCODE_T: return KeyCode::T;
        case SDL_SCANCODE_U: return KeyCode::U;
        case SDL_SCANCODE_V: return KeyCode::V;
        case SDL_SCANCODE_W: return KeyCode::W;
        case SDL_SCANCODE_X: return KeyCode::X;
        case SDL_SCANCODE_Y: return KeyCode::Y;
        case SDL_SCANCODE_Z: return KeyCode::Z;

        // Number row
        case SDL_SCANCODE_0: return KeyCode::KEY_0;
        case SDL_SCANCODE_1: return KeyCode::KEY_1;
        case SDL_SCANCODE_2: return KeyCode::KEY_2;
        case SDL_SCANCODE_3: return KeyCode::KEY_3;
        case SDL_SCANCODE_4: return KeyCode::KEY_4;
        case SDL_SCANCODE_5: return KeyCode::KEY_5;
        case SDL_SCANCODE_6: return KeyCode::KEY_6;
        case SDL_SCANCODE_7: return KeyCode::KEY_7;
        case SDL_SCANCODE_8: return KeyCode::KEY_8;
        case SDL_SCANCODE_9: return KeyCode::KEY_9;

        // Punctuation
        case SDL_SCANCODE_SPACE:        return KeyCode::SPACE;
        case SDL_SCANCODE_APOSTROPHE:   return KeyCode::APOSTROPHE;
        case SDL_SCANCODE_COMMA:        return KeyCode::COMMA;
        case SDL_SCANCODE_MINUS:        return KeyCode::MINUS;
        case SDL_SCANCODE_PERIOD:       return KeyCode::PERIOD;
        case SDL_SCANCODE_SLASH:        return KeyCode::SLASH;
        case SDL_SCANCODE_SEMICOLON:    return KeyCode::SEMICOLON;
        case SDL_SCANCODE_EQUALS:       return KeyCode::EQUAL;
        case SDL_SCANCODE_LEFTBRACKET:  return KeyCode::LEFT_BRACKET;
        case SDL_SCANCODE_BACKSLASH:    return KeyCode::BACKSLASH;
        case SDL_SCANCODE_RIGHTBRACKET: return KeyCode::RIGHT_BRACKET;
        case SDL_SCANCODE_GRAVE:        return KeyCode::GRAVE_ACCENT;

        // Editing / navigation
        case SDL_SCANCODE_ESCAPE:    return KeyCode::ESCAPE;
        case SDL_SCANCODE_RETURN:    return KeyCode::ENTER;
        case SDL_SCANCODE_TAB:       return KeyCode::TAB;
        case SDL_SCANCODE_BACKSPACE: return KeyCode::BACKSPACE;
        case SDL_SCANCODE_INSERT:    return KeyCode::INSERT;
        case SDL_SCANCODE_DELETE:    return KeyCode::DELETE;
        case SDL_SCANCODE_RIGHT:     return KeyCode::RIGHT;
        case SDL_SCANCODE_LEFT:      return KeyCode::LEFT;
        case SDL_SCANCODE_DOWN:      return KeyCode::DOWN;
        case SDL_SCANCODE_UP:        return KeyCode::UP;
        case SDL_SCANCODE_PAGEUP:    return KeyCode::PAGE_UP;
        case SDL_SCANCODE_PAGEDOWN:  return KeyCode::PAGE_DOWN;
        case SDL_SCANCODE_HOME:      return KeyCode::HOME;
        case SDL_SCANCODE_END:       return KeyCode::END;

        // Locks / system
        case SDL_SCANCODE_CAPSLOCK:    return KeyCode::CAPS_LOCK;
        case SDL_SCANCODE_SCROLLLOCK:  return KeyCode::SCROLL_LOCK;
        case SDL_SCANCODE_NUMLOCKCLEAR:return KeyCode::NUM_LOCK;
        case SDL_SCANCODE_PRINTSCREEN: return KeyCode::PRINT_SCREEN;
        case SDL_SCANCODE_PAUSE:       return KeyCode::PAUSE;

        // Function keys
        case SDL_SCANCODE_F1:  return KeyCode::F1;
        case SDL_SCANCODE_F2:  return KeyCode::F2;
        case SDL_SCANCODE_F3:  return KeyCode::F3;
        case SDL_SCANCODE_F4:  return KeyCode::F4;
        case SDL_SCANCODE_F5:  return KeyCode::F5;
        case SDL_SCANCODE_F6:  return KeyCode::F6;
        case SDL_SCANCODE_F7:  return KeyCode::F7;
        case SDL_SCANCODE_F8:  return KeyCode::F8;
        case SDL_SCANCODE_F9:  return KeyCode::F9;
        case SDL_SCANCODE_F10: return KeyCode::F10;
        case SDL_SCANCODE_F11: return KeyCode::F11;
        case SDL_SCANCODE_F12: return KeyCode::F12;

        // Modifiers
        case SDL_SCANCODE_LSHIFT: return KeyCode::LEFT_SHIFT;
        case SDL_SCANCODE_LCTRL:  return KeyCode::LEFT_CONTROL;
        case SDL_SCANCODE_LALT:   return KeyCode::LEFT_ALT;
        case SDL_SCANCODE_LGUI:   return KeyCode::LEFT_SUPER;
        case SDL_SCANCODE_RSHIFT: return KeyCode::RIGHT_SHIFT;
        case SDL_SCANCODE_RCTRL:  return KeyCode::RIGHT_CONTROL;
        case SDL_SCANCODE_RALT:   return KeyCode::RIGHT_ALT;
        case SDL_SCANCODE_RGUI:   return KeyCode::RIGHT_SUPER;

        default: return KeyCode::UNKNOWN;
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

    // Enable Unicode text input so SDL_EVENT_TEXT_INPUT flows to text fields (see update()).
    SDL_StartTextInput(window_handle);
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

                for (const auto& callback : resize_callbacks_)
                {
                    callback(properties_.extent);
                }
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

                // Esc requests UI mode (frees the cursor); the reconcile below enacts it. The app
                // (a UI pass) requests game mode again (e.g. a click on empty world).
                if (down && event.key.scancode == SDL_SCANCODE_ESCAPE)
                    input_.set_capture_requested(false);
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:
            {
                // Only feed look-deltas while captured, so a released cursor doesn't turn.
                if (input_.mouse_captured())
                    input_.add_mouse_delta({ event.motion.xrel, event.motion.yrel });
                // Absolute position (window pixels) for UI hit-testing — meaningful when released.
                input_.set_mouse_position({ event.motion.x, event.motion.y });
                break;
            }
            case SDL_EVENT_TEXT_INPUT:
            {
                // Composed Unicode text (UTF-8), for text fields. SDL text input is started at
                // window creation so these always flow (see the SDL_StartTextInput call there).
                input_.add_typed_text(event.text.text);
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            {
                const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                input_.set_mouse_button(translate_button(event.button.button), down);
                // Capture (game vs UI mode) is now app-driven: a UI pass decides whether a click
                // focuses UI or requests game mode. See the reconcile at the end of update().
                break;
            }
        }
    }

    // Reconcile the actual cursor mode to what the app requested this frame (Esc / a UI click set
    // the intent). Doing it once here — not per event — keeps game/UI mode fully app-driven.
    if (input_.capture_requested() != input_.mouse_captured())
    {
        SDL_SetWindowRelativeMouseMode((SDL_Window*)window_handle_, input_.capture_requested());
        input_.set_mouse_captured(input_.capture_requested());
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