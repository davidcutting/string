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
#include <string/platform/wayland/client.hpp>
#include <string/logger.hpp>

int main(int argc, char *argv[])
{
    STRING_LOG_INFO("Starting up!");

    using namespace String::Wayland;
    
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
