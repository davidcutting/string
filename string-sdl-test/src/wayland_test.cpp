#include <cstdint>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <linux/input-event-codes.h>
#include <xdg-shell-client-protocol.h>

#include <string/platform/wayland/shm.hpp>
#include <string/logger.hpp>

static const int image_width = 128;
static const int image_height = 128;

static void noop(auto...)
{
    // This space intentionally left blank
}

struct wl_context {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* xdg_wm_base = nullptr;
    
    wl_shm* shm = nullptr;

    bool running = true;

    const xdg_wm_base_listener xdg_wm_base_listener = {
        .ping = [](void*, struct xdg_wm_base* xdg_wm_base, uint32_t serial) {
            xdg_wm_base_pong(xdg_wm_base, serial);
        }
    };

    const wl_registry_listener registry_listener = {
        .global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
            auto* self = static_cast<wl_context*>(data);
            if (strcmp(interface, wl_compositor_interface.name) == 0)
                self->compositor = static_cast<wl_compositor*>(
                    wl_registry_bind(registry, name, &wl_compositor_interface, 4));
            else if (strcmp(interface, wl_shm_interface.name) == 0)
                self->shm = static_cast<wl_shm*>(
                    wl_registry_bind(registry, name, &wl_shm_interface, 1));
            else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
                self->xdg_wm_base = static_cast<struct xdg_wm_base*>(
                    wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
                xdg_wm_base_add_listener(self->xdg_wm_base, &self->xdg_wm_base_listener, self);
            }
        },
        .global_remove = noop,
    };

    auto create_buffer(const void* pixel_data, int width, int height) -> wl_buffer*
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
};

struct wl_window {
    wl_context* context = nullptr;

    wl_surface* wl_surface = nullptr;
    xdg_surface* xdg_surface = nullptr;
    xdg_toplevel* xdg_toplevel = nullptr;
    bool configured = false;

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
            self->context->running = false;
        }
    };
};

int main(int argc, char *argv[])
{
    STRING_LOG_INFO("Starting up!");
    
    // Context
    wl_context context;
    context.display = wl_display_connect(NULL);

    if (context.display == nullptr)
    {
    	  STRING_LOG_CRITICAL("Failed to connect to Wayland display!");
        return EXIT_FAILURE;
    }

    context.registry = wl_display_get_registry(context.display);
    wl_registry_add_listener(context.registry, &context.registry_listener, &context);

    if (wl_display_roundtrip(context.display) == -1)
    {
    	  STRING_LOG_CRITICAL("Failed to get Wayland registry!");
        return EXIT_FAILURE;
    }

    // Check that all globals we require are available
    if (context.shm == nullptr || context.compositor == nullptr || context.xdg_wm_base == nullptr)
    {
    	  STRING_LOG_CRITICAL("Required Wayland global object are not available!");
        return EXIT_FAILURE;
    }


    // Window
    wl_window window;
    window.wl_surface = wl_compositor_create_surface(context.compositor);
    window.xdg_surface = xdg_wm_base_get_xdg_surface(context.xdg_wm_base, window.wl_surface);
    window.xdg_toplevel = xdg_surface_get_toplevel(window.xdg_surface);

    xdg_surface_add_listener(window.xdg_surface, &window.xdg_surface_listener, &window);
    xdg_toplevel_add_listener(window.xdg_toplevel, &window.xdg_toplevel_listener, &window);

    // Perform the initial commit and wait for the first configure event
    wl_surface_commit(window.wl_surface);
    while (wl_display_dispatch(context.display) != -1 && !window.configured)
    {
        // This space intentionally left blank
    }

    std::vector<uint8_t> image(image_width * image_height * 4, 0xFF);
    struct wl_buffer *buffer = context.create_buffer(image.data(), image_width, image_height);
    if (buffer == NULL)
    {
        return EXIT_FAILURE;
    }

    wl_surface_attach(window.wl_surface, buffer, 0, 0);
    wl_surface_commit(window.wl_surface);

    // Continue dispatching events until the user closes the toplevel
    while (wl_display_dispatch(context.display) != -1 && context.running)
    {
        // This space intentionally left blank
    }

    STRING_LOG_INFO("Shutting down...");

    xdg_toplevel_destroy(window.xdg_toplevel);
    xdg_surface_destroy(window.xdg_surface);
    wl_surface_destroy(window.wl_surface);
    wl_buffer_destroy(buffer);

    return EXIT_SUCCESS;
}
