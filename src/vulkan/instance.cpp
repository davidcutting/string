#include <GLFW/glfw3.h>

#include <string/core/debug.hpp>
#include <string/vulkan/instance.hpp>
#include <vector>

namespace String {
namespace Vulkan {

Instance::Instance(const std::string& application_name, const Version& application_version,
                   const std::vector<const char*>& required_extensions,
                   const std::vector<const char*>& required_layers) {
    // Check for supported extensions against list of provided extensions we require to be enabled
    get_supported_extensions();
    for (const char* extension_name : required_extensions) {
        if (is_extension_supported(extension_name)) {
            enabled_extensions_.push_back(extension_name);
        }
    }

    // Check for supported validation layers against list of provided layers we require to be enabled
    get_supported_layers();
    for (const char* layer_name : required_layers) {
        if (is_layer_supported(layer_name)) {
            enabled_layers_.push_back(layer_name);
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

    // Set up DebugUtils to get validation messages during instance creation
    // and reuse to create main debug messenger.
    VkDebugUtilsMessengerCreateInfoEXT debug_create_info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_callback,
        .pUserData = nullptr,
    };

    // Create Vulkan Instance
    VkInstanceCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = (VkDebugUtilsMessengerCreateInfoEXT*)&debug_create_info,
        .flags = 0,
        .pApplicationInfo = &application_info,
        .enabledLayerCount = (uint32_t)enabled_layers_.size(),
        .ppEnabledLayerNames = enabled_layers_.data(),
        .enabledExtensionCount = (uint32_t)enabled_extensions_.size(),
        .ppEnabledExtensionNames = enabled_extensions_.data(),
    };

    // TODO: Use custom allocator
    const auto instance_result = vkCreateInstance(&create_info, nullptr, &instance_handle_);
    STRING_ASSERT(instance_result == VK_SUCCESS);

    // Create vulkan debugger
    const auto debug_result =
        create_debug_utils_messenger_ext(instance_handle_, &debug_create_info, nullptr, &debug_utils_messenger_);
    STRING_ASSERT(debug_result == VK_SUCCESS);
}

Instance::Instance(VkInstance instance) : instance_handle_(instance) {}

Instance::~Instance() {
    destroy_debug_utils_messenger_ext(instance_handle_, debug_utils_messenger_, nullptr);
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

const std::vector<const char*>& Instance::get_extensions() const noexcept { return enabled_extensions_; }

void Instance::get_supported_layers() noexcept {
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);

    available_validation_layers_.reserve(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, available_validation_layers_.data());
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

    available_instance_extensions_.reserve(extension_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, available_instance_extensions_.data());
}

bool Instance::is_extension_supported(const char* extension_name) const noexcept {
    for (const auto& extension_properties : available_instance_extensions_) {
        if (strcmp(extension_name, extension_properties.extensionName) == 0) {
            return true;
        }
    }

    return false;
}

VkResult Instance::create_debug_utils_messenger_ext(VkInstance instance,
                                                    const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator,
                                                    VkDebugUtilsMessengerEXT* pDebugMessenger) noexcept {
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    } else {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

void Instance::destroy_debug_utils_messenger_ext(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger,
                                                 const VkAllocationCallbacks* pAllocator) noexcept {
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func != nullptr) {
        func(instance, debugMessenger, pAllocator);
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL Instance::debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                                     VkDebugUtilsMessageTypeFlagsEXT /*messageType*/,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                     void* /*pUserData*/) {
    switch (messageSeverity) {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT: {
            STRING_LOG_ERROR(pCallbackData->pMessage);
            break;
        }
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT: {
            STRING_LOG_INFO(pCallbackData->pMessage);
            break;
        }
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT: {
            STRING_LOG_WARN(pCallbackData->pMessage);
            break;
        }
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT: {
            STRING_LOG_TRACE(pCallbackData->pMessage);
            break;
        }
        default:
            STRING_LOG_INFO(pCallbackData->pMessage);
    }
    return VK_FALSE;
}

}  // namespace Vulkan
}  // namespace String