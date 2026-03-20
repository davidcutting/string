#include <string/vulkan/driver.hpp>
#include <string/vulkan/vulkan_utils.hpp>

#include <string.h>

#define VOLK_STATIC_DISPATCH
#define VOLK_IMPLEMENTATION
#include <volk.h>

namespace
{

bool are_validation_layer_supported(const std::vector<const char*>& validation_layers)
{
    uint32_t layer_count;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);

    std::vector<VkLayerProperties> available_layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data());

    for (const char* layer_name : validation_layers) {
        bool layer_found = false;

        for (const auto& layerProperties : available_layers) {
            if (strcmp(layer_name, layerProperties.layerName) == 0) {
                layer_found = true;
                break;
            }
        }

        if (!layer_found) {
            return false;
        }
    }

    return true;
}

}

namespace String
{

Driver::Driver(const ApplicationInfo& info, const std::shared_ptr<Window>& window)
{
    if (volkInitialize() != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to initialize Volk!");
    }

    bool enable_validation_layers = false;
#ifdef STRING_DEBUG
    enable_validation_layers = true;
    const std::vector<const char*> validation_layers = {"VK_LAYER_KHRONOS_validation"};
    if (!are_validation_layer_supported(validation_layers))
    {
        throw std::runtime_error("Validation layers requested but are not available!");
    }
#endif

    // clang-format off
    VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = info.application_name.c_str(),
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "String Engine",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    // clang-format on

    auto extensions = window->get_platform_extensions(enable_validation_layers);
    extensions.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

    // clang-format off
    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
        .pApplicationInfo = &application_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data()
    };
    // clang-format on

#ifdef STRING_DEBUG
    VkDebugUtilsMessengerCreateInfoEXT instance_debug_create_info{};
    create_info.enabledLayerCount = static_cast<uint32_t>(validation_layers.size());
    create_info.ppEnabledLayerNames = validation_layers.data();

    vku::default_debug_messenger_create_info(instance_debug_create_info);
    create_info.pNext = (VkDebugUtilsMessengerCreateInfoEXT*)&instance_debug_create_info;
#endif

    if (vkCreateInstance(&create_info, nullptr, &instance_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Vulkan instance!");
    }

    volkLoadInstance(instance_);

#ifdef STRING_DEBUG
    VkDebugUtilsMessengerCreateInfoEXT debug_create_info;
    vku::default_debug_messenger_create_info(debug_create_info);

    if (vku::CreateDebugUtilsMessengerEXT(instance_, &debug_create_info, nullptr, &debug_messenger_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to set up Vulkan debug messenger!");
    }
#endif
}

Driver::~Driver()
{
    if (debug_messenger_ != VK_NULL_HANDLE)
        vku::DestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
    if (instance_ != VK_NULL_HANDLE)
        vkDestroyInstance(instance_, nullptr);
}

auto Driver::get_instance() -> VkInstance&
{
    return instance_;
}

}
