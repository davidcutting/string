#include <stdexcept>
#include <string/platform/wayland/client.hpp>
#include <wayland-client-protocol.h>
#include <xdg-shell-client-protocol.h>

#include <string_view>
#include "string/platform/wayland/presenter.hpp"
#include "string/platform/wayland/window.hpp"

static void wl_registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    auto* self = static_cast<wl::client*>(data);
    const auto interface_name = std::string_view(interface);
    if (interface_name == wl_compositor_interface.name)
        self->compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    else if (interface_name == wl_shm_interface.name)
        self->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    else if (interface_name == xdg_wm_base_interface.name)
    {
        self->xdg_wm_base = static_cast<struct xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(self->xdg_wm_base, &self->xdg_wm_base_listener, self);
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

    STRING_LOG_DEBUG("Window successfully created.");

    return new_window;
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