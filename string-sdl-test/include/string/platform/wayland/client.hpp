#pragma once

#include <cstdint>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <linux/input-event-codes.h>
#include <xdg-shell-client-protocol.h>

#include <string/platform/wayland/window.hpp>
#include <string/logger.hpp>
#include <string/platform/wayland/presenter.hpp>

static void wl_registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version);
static void wl_registry_global_remove(void*, struct wl_registry*, uint32_t);
static void xdg_wm_base_ping(void* data, struct xdg_wm_base* xdg_wm_base, uint32_t serial);

namespace wl
{

class client
{
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* xdg_wm_base = nullptr;
    wl_seat* seat = nullptr;
    wl_shm* shm = nullptr;

    const wl_registry_listener registry_listener;
    const xdg_wm_base_listener xdg_wm_base_listener;
    const wl_seat_listener wl_seat_listener;

public:
    explicit client();
    ~client();

    bool poll_events();
    auto create_window() -> window*;
    auto create_presenter(const window* window) -> presenter*;

private:
    // Listener callbacks
    friend void ::wl_registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version);
    friend void ::wl_registry_global_remove(void*, struct wl_registry*, uint32_t);
    friend void ::xdg_wm_base_ping(void* data, struct xdg_wm_base* xdg_wm_base, uint32_t serial);
    
};

}