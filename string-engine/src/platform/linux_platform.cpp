#include <memory>
#include <string/platform/platform.hpp>
#include <string/platform/window.hpp>
#include <string/logger.hpp>
#define VOLK_IMPLEMENTATION
#include "volk.h"

namespace String
{

Platform::Platform(const PlatformInfo& platform_info)
{
    create_instance();
    setup_debug_messenger();
}

Platform::~Platform()
{
    if (debug_messenger_ != VK_NULL_HANDLE)
        DestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
    if (instance_ != VK_NULL_HANDLE)
        vkDestroyInstance(instance_, nullptr);
}

auto Platform::create_window(const WindowInfo& info) -> std::expected<std::unique_ptr<Window>, PlatformError>
{
    auto window = std::make_unique<Window>(info);
    return window;
}

auto Platform::create_device(const Window* window) -> std::expected<std::unique_ptr<Device>, PlatformError>
{
    if (!window)
    {
        STRING_LOG_ERROR("Failed to create device: Window does not exist!");
        return std::unexpected<PlatformError>(PlatformError::DeviceInitFailed);
    }
    const auto device_info = DeviceInfo {
        .instance = instance_,
        .surface = window->get_vulkan_surface(instance_),
    };
    try
    {
        return std::move(std::make_unique<Device>(device_info));
    }
    catch (const std::exception& e)
    {
        STRING_LOG_ERROR("Failed to create device: ", e.what());
        return std::unexpected<PlatformError>(PlatformError::DeviceInitFailed);
    }
}

bool Platform::poll_events()
{
    return true;
}

auto Platform::get_platform_extensions() -> std::vector<const char*>
{
    return {
        "VK_KHR_surface",
        "VK_KHR_wayland_surface",
    };
}

}