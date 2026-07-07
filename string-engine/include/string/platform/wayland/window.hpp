#pragma once

#include <xdg-shell-client-protocol.h>
#include <wayland-client-protocol.h>

#include <cstdint>
#include <functional>


static void xdg_surface_configure(void* data, xdg_surface* surface, uint32_t serial);
static void xdg_toplevel_configure(void* data, xdg_toplevel* toplevel, int32_t width, int32_t height, wl_array*);
static void xdg_toplevel_close(void* data, xdg_toplevel* toplevel);

namespace wl
{

using on_resize = std::function<void(std::uint32_t, uint32_t)>;
using on_close = std::function<void()>;

class window
{
    wl_surface* wl_surface = nullptr;
    xdg_surface* xdg_surface = nullptr;
    xdg_toplevel* xdg_toplevel = nullptr;

    const xdg_surface_listener xdg_surface_listener;
    const xdg_toplevel_listener xdg_toplevel_listener;

    bool configured = false;
    bool open = true;
    uint32_t width = 800;
    uint32_t height = 600;

    on_resize resize_callback;
    on_close close_callback;

public:
    window(wl_compositor* compositor, xdg_wm_base* xdg_wm_base);
    ~window();

    bool is_configured() const;
    bool should_close() const;
    void close();
    auto get_surface() const -> struct wl_surface*;
    auto get_xdg_surface() const -> struct xdg_surface*;

private:
    friend class client;
    friend void ::xdg_surface_configure(void* data, struct xdg_surface* surface, uint32_t serial);
    friend void ::xdg_toplevel_configure(void* data, struct xdg_toplevel* toplevel, int32_t width, int32_t height, wl_array*);
    friend void ::xdg_toplevel_close(void* data, struct xdg_toplevel* toplevel);
};

}