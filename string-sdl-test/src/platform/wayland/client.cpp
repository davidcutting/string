#include <string/platform/wayland/client.hpp>
#include <string/logger.hpp>
#include <string/platform/wayland/presenter.hpp>
#include <string/platform/wayland/window.hpp>
#include <wayland-client-protocol.h>
#include <xdg-shell-client-protocol.h>

#include <algorithm>
#include <stdexcept>
#include <string_view>

static void wl_registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    auto* self = static_cast<wl::client*>(data);
    const auto interface_name = std::string_view(interface);
    if (interface_name == wl_compositor_interface.name)
    {
        self->compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    }
    else if (interface_name == wl_shm_interface.name)
    {
        self->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    }
    else if (interface_name == xdg_wm_base_interface.name)
    {
        self->xdg_wm_base = static_cast<struct xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(self->xdg_wm_base, &self->xdg_wm_base_listener, self);
    }
    else if (interface_name == wl_seat_interface.name)
    {
        self->seat = static_cast<struct wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        wl_seat_add_listener(self->seat, &self->wl_seat_listener, self);
    }
}

static void wl_registry_global_remove(void*, struct wl_registry*, uint32_t)
{
    // no-op
}

static void xdg_wm_base_ping(void* data, struct xdg_wm_base* xdg_wm_base, uint32_t serial)
{
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static void wl_seat_capabilities(void* data, struct wl_seat* wl_seat, uint32_t capabilities)
{
    auto* self = static_cast<wl::client*>(data);
    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD && !self->keyboard)
    {
        self->keyboard = wl_seat_get_keyboard(wl_seat);
        wl_keyboard_add_listener(self->keyboard, &self->wl_keyboard_listener, self);
    }
    if (capabilities & WL_SEAT_CAPABILITY_POINTER && !self->pointer)
    {
        self->pointer = wl_seat_get_pointer(wl_seat);
        wl_pointer_add_listener(self->pointer, &self->wl_pointer_listener, self);
    }
}

static void wl_seat_name(void* data, struct wl_seat* wl_seat, const char* name)
{
    // no-op
}

static void wl_pointer_enter(void* data, wl_pointer* pointer, uint32_t serial, wl_surface* surface, wl_fixed_t, wl_fixed_t)
{
    auto* self = static_cast<wl::client*>(data);

    // TODO: Maybe we should still track pointer even though the window isn't focused?
    if (!self->focused_window)
    {
        return;
    }

    const auto window_iter = std::find_if(self->open_windows.begin(), self->open_windows.end(), [surface](wl::window* window){
        return window->get_surface() == surface;
    });

    if (window_iter != self->open_windows.end())
    {
        STRING_LOG_DEBUG("Pointer entering window: {}", (void*)*window_iter);
        self->pointer_in_window = true;
        return;
    }
    
    STRING_LOG_WARN("Pointer has entered an unknown surface...?");
}

static void wl_pointer_leave(void* data, wl_pointer* pointer, uint32_t serial, wl_surface* surface)
{
    auto* self = static_cast<wl::client*>(data);
    // TODO: Maybe we should still track pointer even though the window isn't focused?
    if (!self->focused_window)
    {
        return;
    }
    STRING_LOG_DEBUG("Pointer leaving window.");
    self->pointer_in_window = false;
}

static void wl_pointer_motion(void* data, wl_pointer* pointer, uint32_t serial, int, int)
{
    // todo handle motion callback on drag
}

static void wl_pointer_button(void* data, wl_pointer* pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    // todo handle click exit
}

static void wl_pointer_axis(void* data, wl_pointer* pointer, uint32_t serial, uint32_t time, wl_fixed_t)
{
    // no-op
}

static void wl_keyboard_keymap(void *, struct wl_keyboard *, uint32_t, int32_t, uint32_t)
{
    // no-op
}

static void wl_keyboard_enter(void* data, wl_keyboard*, uint32_t, wl_surface* surface, wl_array*)
{
    auto* self = static_cast<wl::client*>(data);

    const auto window_iter = std::find_if(self->open_windows.begin(), self->open_windows.end(), [surface](wl::window* window){
        return window->get_surface() == surface;
    });

    if (window_iter != self->open_windows.end())
    {
        STRING_LOG_DEBUG("Focusing window: {}", (void*)*window_iter);
        self->focused_window = *window_iter;
        return;
    }
    
    STRING_LOG_WARN("An unknown surface has been focused...?");
}

static void wl_keyboard_leave(void* data, wl_keyboard*, uint32_t, struct wl_surface*)
{
    auto* self = static_cast<wl::client*>(data);
    STRING_LOG_DEBUG("Window losing focus.");
    self->focused_window = nullptr;
}

static void wl_keyboard_key(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    auto* self = static_cast<wl::client*>(data);
    if (self->focused_window == nullptr)
    {
        STRING_LOG_DEBUG("Received key {} while no focused window... Huh??", key);
        return;
    }
    switch (key)
    {
        case 1:
            self->focused_window->close();
            return;
        default:
            STRING_LOG_DEBUG("Pressed key: {}", key);
    }
}

static void wl_keyboard_modifiers(void *, struct wl_keyboard *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
{
    // no-op
}

static void wl_keyboard_repeat_info(void *, struct wl_keyboard *, int32_t, int32_t)
{
    // no-op
}

namespace wl
{

client::client()
:   registry_listener{
        .global = wl_registry_global,
        .global_remove = wl_registry_global_remove,
    }
,   xdg_wm_base_listener{
        .ping = xdg_wm_base_ping,
    }
,   wl_seat_listener{
        .capabilities = wl_seat_capabilities,
        .name = wl_seat_name,
    }
,   wl_keyboard_listener{
        .keymap = wl_keyboard_keymap,
        .enter = wl_keyboard_enter,
        .leave = wl_keyboard_leave,
        .key = wl_keyboard_key,
        .modifiers = wl_keyboard_modifiers,
        .repeat_info = wl_keyboard_repeat_info,
    }
,   wl_pointer_listener{
        .enter = wl_pointer_enter,
        .leave = wl_pointer_leave,
        .motion = wl_pointer_motion,
        .button = wl_pointer_button,
        .axis = wl_pointer_axis,
    }
{
    STRING_LOG_DEBUG("Connecting to display...");

    display = wl_display_connect(nullptr);
    if (!display)
    {
        STRING_LOG_CRITICAL("Failed to connect to Wayland display!");
        throw std::runtime_error("Failed to connect to Wayland display!");
    }

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, this);

    if (wl_display_roundtrip(display) == -1)
    {
    	STRING_LOG_CRITICAL("Failed to rountrip display on Wayland client initialization.");
        throw std::runtime_error("Failed to rountrip display on Wayland client initialization.");
    }

    if (shm == nullptr || compositor == nullptr || xdg_wm_base == nullptr)
    {
    	STRING_LOG_CRITICAL("Required Wayland global object are not available!");
        throw std::runtime_error("Required Wayland global object are not available!");
    }
    STRING_LOG_DEBUG("Client successfully created.");
}

client::~client()
{
    STRING_LOG_DEBUG("Closing Wayland client.");
    if (registry) (void)registry;
    if (display) wl_display_disconnect(display);
}

auto client::create_window() -> window*
{
    auto* new_window = new window(compositor, xdg_wm_base);

    while (poll_events() && !new_window->is_configured())
    {
        // busy loop until the window is configured
    }

    STRING_LOG_DEBUG("Window successfully created: {}", (void*)new_window);

    open_windows.push_back(new_window);

    return new_window;
}

void client::delete_window(window* window)
{
    auto erased = std::erase_if(open_windows, [window](wl::window* in_vector){
        return window == in_vector;
    });

    if (erased < 1)
    {
        STRING_LOG_WARN("Erased {} windows when delete_window called. What???", erased);
    }

    STRING_LOG_DEBUG("Deleting window: {}", (void*)window);
}

bool client::poll_events()
{
    return wl_display_dispatch(display) > 0;
}

auto client::create_presenter(const window* window) -> presenter*
{
    return new presenter(shm, window->wl_surface, window->width, window->height);
}

};