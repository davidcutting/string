#include <cstdint>
#include <string/platform/wayland/window.hpp>
#include "string/logger.hpp"
#include <xdg-shell-client-protocol.h>

static void xdg_surface_configure(void* data, xdg_surface* surface, uint32_t serial)
{
    auto* self = static_cast<wl::window*>(data);
    xdg_surface_ack_configure(surface, serial);
    if (self->configured)
        wl_surface_commit(self->wl_surface);
    self->configured = true;
}

static void xdg_toplevel_configure(void* data, xdg_toplevel* toplevel, int32_t width, int32_t height, wl_array*)
{
    auto* self = static_cast<wl::window*>(data);
    if (self->width != width || self->height != height)
    {
        if (self->resize_callback)
            std::invoke(self->resize_callback, width, height);
    }
}

static void xdg_toplevel_close(void* data, xdg_toplevel* toplevel)
{
    auto* self = static_cast<wl::window*>(data);
    self->open = false;
    if (self->close_callback)
        std::invoke(self->close_callback);
}

namespace wl
{

window::window(wl_compositor* compositor, xdg_wm_base* xdg_wm_base)
: xdg_surface_listener{
    .configure = xdg_surface_configure,
}
, xdg_toplevel_listener{
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
}
{
    STRING_LOG_DEBUG("Opening a window...");

    wl_surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, wl_surface);
    xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);

    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, this);
    xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, this);

    // Perform the initial commit
    wl_surface_commit(wl_surface);

    STRING_LOG_DEBUG("Window surface committed...");
}

window::~window()
{
    STRING_LOG_DEBUG("Closing Wayland window.");
    if (xdg_toplevel) xdg_toplevel_destroy(xdg_toplevel);
    if (xdg_surface) xdg_surface_destroy(xdg_surface);
    if (wl_surface) wl_surface_destroy(wl_surface);
}

bool window::is_configured() const
{
    return configured;
}

bool window::should_close() const
{
    return !open;
}

void window::close()
{
    open = false;
}

auto window::get_surface() const -> struct wl_surface*
{
    return wl_surface;
}

auto window::get_xdg_surface() const -> struct xdg_surface*
{
    return xdg_surface;
}

};