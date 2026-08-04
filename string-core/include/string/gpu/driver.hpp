#pragma once

#include <string/core/logger.hpp>
#include <string/core/platform_detection.hpp>
#include <string/core/app_info.hpp>
#include <string/platform/window.hpp>

#include <volk.h>

namespace string::gpu
{

class driver
{
    VkInstance instance_;
    VkDebugUtilsMessengerEXT debug_messenger_;
public:
    explicit driver(const String::ApplicationInfo& info, const std::shared_ptr<String::Window>& window);
    ~driver();

    auto get_instance() -> VkInstance&;
};

}