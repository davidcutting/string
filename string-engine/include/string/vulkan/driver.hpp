#pragma once

#include <string/core/logger.hpp>
#include <string/core/platform_detection.hpp>
#include <string/core/app_info.hpp>
#include <string/platform/window.hpp>

#include <volk.h>

namespace String
{

class Driver
{
    VkInstance instance_;
    VkDebugUtilsMessengerEXT debug_messenger_;
public:
    explicit Driver(const ApplicationInfo& info, const std::shared_ptr<Window>& window);
    ~Driver();

    auto get_instance() -> VkInstance&;
};

}