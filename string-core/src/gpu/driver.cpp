#include <string/gpu/driver.hpp>
#include <string/core/logger.hpp>
#include <string/vulkan/vulkan_utils.hpp>

#include <cstdlib>
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

namespace string::gpu
{

driver::driver(const string::ApplicationInfo& info, const std::shared_ptr<string::Window>& window)
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

    // VK_KHR_portability_enumeration is only needed for portability drivers (e.g. MoltenVK) and is
    // NOT exposed by some capture layers (RenderDoc), which otherwise fails to attach. Request it —
    // and the matching create flag — ONLY when the loader actually reports it available.
    bool has_portability = false;
    {
        uint32_t ext_count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> props(ext_count);
        vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, props.data());
        for (const VkExtensionProperties& p : props)
        {
            if (strcmp(p.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0)
            {
                has_portability = true;
                extensions.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
                break;
            }
        }
    }

    // clang-format off
    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = has_portability
                     ? static_cast<VkInstanceCreateFlags>(VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR)
                     : 0u,
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

    string::vku::default_debug_messenger_create_info(instance_debug_create_info);
    create_info.pNext = (VkDebugUtilsMessengerCreateInfoEXT*)&instance_debug_create_info;

    // STRING_SYNC_VALIDATION=1: turn on SYNCHRONIZATION validation.
    //
    // This is OPT-IN and separate from the default checks, which is easy to be caught out by: a
    // clean validation run says the API usage is LEGAL, not that the synchronisation is CORRECT.
    // Missing barriers are invisible to the default layer set, and they are precisely the bug class
    // that renders correctly on one vendor and produces garbage on another (whoever's scheduler
    // happens to overlap the unsynchronised work). If output is wrong on one machine and right on
    // another while plain validation stays silent, this is the check that has not been run yet.
    //
    // Not on by default even in debug: it is expensive, and it is a deliberate investigation tool.
    // Kept as an env var rather than a CVar because it must be decided before instance creation,
    // which is well before the CVar registry exists.
    VkValidationFeaturesEXT validation_features{ VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT };
    const VkValidationFeatureEnableEXT sync_feature =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    if (const char* sv = std::getenv("STRING_SYNC_VALIDATION"); sv != nullptr && sv[0] == '1')
    {
        validation_features.enabledValidationFeatureCount = 1;
        validation_features.pEnabledValidationFeatures = &sync_feature;
        validation_features.pNext = create_info.pNext;   // keep the debug messenger in the chain
        create_info.pNext = &validation_features;
        STRING_LOG_INFO("Vulkan SYNCHRONIZATION validation ENABLED (STRING_SYNC_VALIDATION=1)");
    }
#endif

    if (vkCreateInstance(&create_info, nullptr, &instance_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Vulkan instance!");
    }

    volkLoadInstance(instance_);

#ifdef STRING_DEBUG
    VkDebugUtilsMessengerCreateInfoEXT debug_create_info;
    string::vku::default_debug_messenger_create_info(debug_create_info);

    if (string::vku::CreateDebugUtilsMessengerEXT(instance_, &debug_create_info, nullptr, &debug_messenger_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to set up Vulkan debug messenger!");
    }
#endif
}

driver::~driver()
{
    if (debug_messenger_ != VK_NULL_HANDLE)
        string::vku::DestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
    if (instance_ != VK_NULL_HANDLE)
        vkDestroyInstance(instance_, nullptr);
}

auto driver::get_instance() -> VkInstance&
{
    return instance_;
}

}
