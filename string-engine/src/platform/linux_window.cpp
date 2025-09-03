#include <stdexcept>
#include <string/platform/window.hpp>

#include "volk.h"
#include <vulkan/vulkan_wayland.h>

namespace String
{

Window::Window(const WindowInfo& window_info)
: window_info_(window_info)
{

}

Window::~Window()
{
}

auto Window::get_window_info() const -> WindowInfo
{
    return window_info_;
}

auto Window::get_native_handle() const -> void*
{
    return nullptr;
}

auto Window::get_vulkan_surface(const VkInstance& instance) const -> VkSurfaceKHR
{
    VkWaylandSurfaceCreateInfoKHR surface_create_info = {
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        // .display = Wl_display,
        // .surface = wl_surface,
    };

    PFN_vkCreateWaylandSurfaceKHR fpCreateWaylandSurfaceKHR =
        (PFN_vkCreateWaylandSurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateWaylandSurfaceKHR");

    VkSurfaceKHR surface;
    if (fpCreateWaylandSurfaceKHR(instance, &surface_create_info, nullptr, &surface) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create Vulkan surface!");
    }

    return surface;
}

auto Window::is_minimized() const -> bool
{
    return false;
}

auto Window::is_maximized() const -> bool
{
    return true;
}

auto Window::has_focus() const -> bool
{
    return true;
}

auto Window::show() -> void
{
}

auto Window::hide() -> void
{
}

}