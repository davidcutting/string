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

static void wl_seat_capabilities(void* data, struct wl_seat* wl_seat, uint32_t capabilities);
static void wl_seat_name(void* data, struct wl_seat* wl_seat, const char* name);

static void wl_keyboard_keymap(void* data, wl_keyboard* keyboard, uint32_t, int32_t, uint32_t);
static void wl_keyboard_enter(void* data, wl_keyboard* keyboard, uint32_t, wl_surface* surface, wl_array*);
static void wl_keyboard_leave(void* data, wl_keyboard* keyboard, uint32_t, wl_surface* surface);
static void wl_keyboard_key(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
static void wl_keyboard_modifiers(void* data, wl_keyboard* keyboard, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static void wl_keyboard_repeat_info(void* data, wl_keyboard* keyboard, int32_t, int32_t);

static void wl_pointer_enter(void* data, wl_pointer* pointer, uint32_t serial, wl_surface* surface, wl_fixed_t, wl_fixed_t);
static void wl_pointer_leave(void* data, wl_pointer* pointer, uint32_t serial, wl_surface* surface);
static void wl_pointer_motion(void* data, wl_pointer* pointer, uint32_t serial, int, int);
static void wl_pointer_button(void* data, wl_pointer* pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
static void wl_pointer_axis(void* data, wl_pointer* pointer, uint32_t serial, uint32_t time, wl_fixed_t);

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
    wl_keyboard* keyboard = nullptr;
    wl_pointer* pointer = nullptr;

    std::vector<window*> open_windows;
    window* focused_window = nullptr;
    bool pointer_in_window = false;

    const wl_registry_listener registry_listener;
    const xdg_wm_base_listener xdg_wm_base_listener;
    const wl_seat_listener wl_seat_listener;
    const wl_keyboard_listener wl_keyboard_listener;
    const wl_pointer_listener wl_pointer_listener;

public:
    explicit client();
    ~client();

    bool poll_events();
    auto create_window() -> window*;
    void delete_window(window* window);
    auto create_presenter(const window* window) -> presenter*;

private:
    // Listener callbacks
    friend void ::wl_registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version);
    friend void ::wl_registry_global_remove(void*, struct wl_registry*, uint32_t);
    friend void ::xdg_wm_base_ping(void* data, struct xdg_wm_base* xdg_wm_base, uint32_t serial);
    friend void ::wl_seat_capabilities(void* data, struct wl_seat* wl_seat, uint32_t capabilities);
    friend void ::wl_seat_name(void* data, struct wl_seat* wl_seat, const char* name);

    friend void ::wl_keyboard_keymap(void *, struct wl_keyboard *, uint32_t, int32_t, uint32_t);
    friend void ::wl_keyboard_enter(void *, struct wl_keyboard *, uint32_t, struct wl_surface *, struct wl_array *);
    friend void ::wl_keyboard_leave(void *, struct wl_keyboard *, uint32_t, struct wl_surface *);
    friend void ::wl_keyboard_key(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
    friend void ::wl_keyboard_modifiers(void *, struct wl_keyboard *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    friend void ::wl_keyboard_repeat_info(void *, struct wl_keyboard *, int32_t, int32_t);

    friend void ::wl_pointer_enter(void* data, wl_pointer* pointer, uint32_t serial, wl_surface* surface, wl_fixed_t, wl_fixed_t);
    friend void ::wl_pointer_leave(void* data, wl_pointer* pointer, uint32_t serial, wl_surface* surface);
    friend void ::wl_pointer_motion(void* data, wl_pointer* pointer, uint32_t serial, int, int);
    friend void ::wl_pointer_button(void* data, wl_pointer* pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
    friend void ::wl_pointer_axis(void* data, wl_pointer* pointer, uint32_t serial, uint32_t time, wl_fixed_t);
};

}