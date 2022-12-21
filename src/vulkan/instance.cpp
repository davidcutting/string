#include <GLFW/glfw3.h>
#include <vulkan/vulkan_core.h>

#include <string/core/debug.hpp>
#include <string/vulkan/instance.hpp>
#include <vector>

namespace String {
namespace Vulkan {

#ifndef STRING_RELEASE
namespace {
VKAPI_ATTR VkBool32 VKAPI_CALL debug_utils_messenger_callback(VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
                                                              VkDebugUtilsMessageTypeFlagsEXT /*message_type*/,
                                                              const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
                                                              void* /*user_data*/) {
    // Log debug messge
    if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        STRING_LOG_TRACE(callback_data->pMessage);
    } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        STRING_LOG_INFO(callback_data->pMessage);
    } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        STRING_LOG_WARN(callback_data->pMessage);
    } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        STRING_LOG_ERROR(callback_data->pMessage);
    }
    return VK_FALSE;
}
/*
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT type,
                                                     uint64_t object, size_t location, int32_t message_code,
                                                     const char* layer_prefix, const char* message,
                                                     void* user_data) {
    if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) {
        STRING_LOG_ERROR("{}: {}", layer_prefix, message);
    } else if (flags & VK_DEBUG_REPORT_WARNING_BIT_EXT) {
        STRING_LOG_WARN("{}: {}", layer_prefix, message);
    } else if (flags & VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT) {
        STRING_LOG_WARN("{}: {}", layer_prefix, message);
    } else {
        STRING_LOG_INFO("{}: {}", layer_prefix, message);
    }
    return VK_FALSE;
} */

VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
                                      const VkAllocationCallbacks* pAllocator,
                                      VkDebugUtilsMessengerEXT* pDebugMessenger) {
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    } else {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger,
                                   const VkAllocationCallbacks* pAllocator) {
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func != nullptr) {
        func(instance, debugMessenger, pAllocator);
    }
}

}  // namespace
#endif

Instance::Instance(const std::string& application_name, const Version& application_version,
                   const std::vector<const char*>& required_extensions,
                   const std::vector<const char*>& required_layers) {
    // Check for supported extensions against list of provided extensions we require to be enabled
    get_supported_extensions();
    for (const char* extension_name : required_extensions) {
        if (is_extension_supported(extension_name)) {
            enabled_extensions_.emplace_back(extension_name);
        }
    }

#ifndef STRING_RELEASE
    enabled_extensions_.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    enabled_layers_.emplace_back("VK_LAYER_KHRONOS_validation");
#endif

    // Check for supported validation layers against list of provided layers we require to be enabled
    get_supported_layers();
    for (const char* layer_name : required_layers) {
        if (is_layer_supported(layer_name)) {
            enabled_layers_.emplace_back(layer_name);
        }
    }

    VkApplicationInfo application_info{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = application_name.c_str(),
        .applicationVersion =
            VK_MAKE_VERSION(application_version.major, application_version.minor, application_version.patch),
        .pEngineName = "String Engine",
        .engineVersion = VK_MAKE_VERSION(0, 0, 1),
        .apiVersion = VK_API_VERSION_1_3,
    };

    // Create Vulkan Instance
    VkInstanceCreateInfo instance_create_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pApplicationInfo = &application_info,
        .enabledLayerCount = (uint32_t)enabled_layers_.size(),
        .ppEnabledLayerNames = enabled_layers_.data(),
        .enabledExtensionCount = (uint32_t)enabled_extensions_.size(),
        .ppEnabledExtensionNames = enabled_extensions_.data(),
    };

#ifndef STRING_RELEASE
    VkDebugUtilsMessengerCreateInfoEXT debug_instance_create_info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_utils_messenger_callback,
        .pUserData = nullptr,
    };

    instance_create_info.pNext = (VkDebugUtilsMessengerCreateInfoEXT*)&debug_instance_create_info;
#endif

    // TODO: Use custom allocator
    const auto instance_result = vkCreateInstance(&instance_create_info, nullptr, &instance_handle_);
    STRING_ASSERT(instance_result == VK_SUCCESS);

#ifndef STRING_RELEASE
    VkDebugUtilsMessengerCreateInfoEXT debug_utils_create_info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_utils_messenger_callback,
        .pUserData = nullptr,
    };

    auto debug_result =
        CreateDebugUtilsMessengerEXT(instance_handle_, &debug_utils_create_info, nullptr, &debug_utils_messenger_);
    STRING_ASSERT(debug_result == VK_SUCCESS);
#endif
}

Instance::Instance(VkInstance instance) : instance_handle_(instance) {}

Instance::~Instance() {
#ifndef STRING_RELEASE
    if (debug_utils_messenger_ != VK_NULL_HANDLE) {
        DestroyDebugUtilsMessengerEXT(instance_handle_, debug_utils_messenger_, nullptr);
    }
#endif
    if (instance_handle_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_handle_, nullptr);
    }
}

VkInstance Instance::get_handle() const noexcept { return instance_handle_; }

bool Instance::is_extension_enabled(const char* extension) const noexcept {
    return std::find_if(enabled_extensions_.begin(), enabled_extensions_.end(),
                        [extension](const char* enabled_extension) {
                            return strcmp(extension, enabled_extension) == 0;
                        }) != enabled_extensions_.end();
}

const std::vector<const char*>& Instance::get_enabled_extensions() const noexcept { return enabled_extensions_; }

const std::vector<const char*>& Instance::get_enabled_layers() const noexcept { return enabled_layers_; }

void Instance::get_supported_layers() noexcept {
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);

    std::vector<VkLayerProperties> layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
    available_validation_layers_ = layers;
}

bool Instance::is_layer_supported(const char* layer_name) const noexcept {
    for (const auto& layer_properties : available_validation_layers_) {
        if (strcmp(layer_name, layer_properties.layerName) == 0) {
            return true;
        }
    }
    return false;
}

void Instance::get_supported_extensions() noexcept {
    uint32_t extension_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);

    std::vector<VkExtensionProperties> extensions(extension_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data());
    available_instance_extensions_ = extensions;
}

bool Instance::is_extension_supported(const char* extension_name) const noexcept {
    for (const auto& extension_properties : available_instance_extensions_) {
        if (strcmp(extension_name, extension_properties.extensionName) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace Vulkan
}  // namespace String