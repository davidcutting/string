#pragma once

#include <cstdint>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>
#include <string_view>
#include <vector>
#include <functional>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <linux/input-event-codes.h>
#include <xdg-shell-client-protocol.h>

#include <string/platform/wayland/shm.hpp>
#include <string/logger.hpp>

namespace String::Wayland
{

static const int image_width = 128;
static const int image_height = 128;

static void noop(auto...){}

using on_resize = std::function<void(const std::uint32_t&, const uint32_t&)>;
using on_close = std::function<void()>;

struct wl_context
{
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* xdg_wm_base = nullptr;
    wl_shm* shm = nullptr;

    const xdg_wm_base_listener xdg_wm_base_listener = {
        .ping = [](void*, struct xdg_wm_base* xdg_wm_base, uint32_t serial) {
            xdg_wm_base_pong(xdg_wm_base, serial);
        }
    };

    const wl_registry_listener registry_listener = {
        .global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
            auto* self = static_cast<wl_context*>(data);
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
        },
        .global_remove = noop,
    };
};

auto create_context() -> wl_context;
void destroy_context();

struct wl_window
{
    wl_surface* wl_surface = nullptr;
    xdg_surface* xdg_surface = nullptr;
    xdg_toplevel* xdg_toplevel = nullptr;
    bool configured = false;
    bool open = true;

    on_resize resize_callback;
    on_close close_callback;

    const xdg_surface_listener xdg_surface_listener = {
        .configure = [](void* data, struct xdg_surface* surf, uint32_t serial) {
            auto* self = static_cast<wl_window*>(data);
            xdg_surface_ack_configure(surf, serial);
            if (self->configured)
                wl_surface_commit(self->wl_surface);
            self->configured = true;
        }
    };

    const xdg_toplevel_listener xdg_toplevel_listener = {
        .configure = [](void* data, struct xdg_toplevel*, int32_t, int32_t, wl_array*) {
            // Optional: resize logic
        },
        .close = [](void* data, struct xdg_toplevel*) {
            auto* self = static_cast<wl_window*>(data);
            self->open = false;
            std::invoke(self->close_callback);
        }
    };
};

auto create_window(const wl_context& context) -> wl_window;
void destroy_window();

inline auto create_buffer(const void* pixel_data, int width, int height, wl_shm* shm) -> wl_buffer*
{
    int stride = width * 4;
    int size = stride * height;

    int fd = create_shm_file(size);
    if (fd < 0)
    {
        STRING_LOG_ERROR("Creating a buffer file for {}B failed", size);
        return NULL;
    }

    void* shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm_data == MAP_FAILED) {
        STRING_LOG_ERROR("mmap failed");
        close(fd);
        return NULL;
    }

    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    close(fd);

    memcpy(shm_data, pixel_data, size);

    return buffer;
}

}
