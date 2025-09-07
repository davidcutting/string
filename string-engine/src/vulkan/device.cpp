#include <memory>
#include <set>
#include <stdexcept>
#include <string/vulkan/device.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include <string/vulkan/command_recorder.hpp>
#include <string/core/logger.hpp>
#include <string/vulkan/queue.hpp>

#include <volk.h>

namespace String
{

Device::Device(const ApplicationInfo& info, const std::shared_ptr<Window>& window)
: window_(window)
{
    create_instance(info);
    setup_debug_messenger();
    create_surface();
    select_physical_device();
    create_logical_device();
    allocator_ = std::make_unique<Allocator>(physical_device_, device_, instance_);
}

Device::~Device()
{
    vkDeviceWaitIdle(device_);
    allocator_.reset();
    if (device_ != VK_NULL_HANDLE)
        vkDestroyDevice(device_, nullptr);
    if (surface_ != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (debug_messenger_ != VK_NULL_HANDLE)
        vku::DestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
    if (instance_ != VK_NULL_HANDLE)
        vkDestroyInstance(instance_, nullptr);
}

void Device::create_instance(const ApplicationInfo& info)
{
    if (volkInitialize() != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to initialize Volk!");
    }

    if (ENABLE_VALIDATION_LAYERS && !are_validation_layer_supported()) {
        throw std::runtime_error("Validation layers requested, but not available!");
    }

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

    auto extensions = window_->get_platform_extensions(ENABLE_VALIDATION_LAYERS);
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

    VkDebugUtilsMessengerCreateInfoEXT debug_create_info{};
    if (ENABLE_VALIDATION_LAYERS) {
        create_info.enabledLayerCount = static_cast<uint32_t>(validation_layers.size());
        create_info.ppEnabledLayerNames = validation_layers.data();

        vku::default_debug_messenger_create_info(debug_create_info);
        create_info.pNext = (VkDebugUtilsMessengerCreateInfoEXT*)&debug_create_info;
    }

    if (vkCreateInstance(&create_info, nullptr, &instance_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Vulkan instance!");
    }

    volkLoadInstance(instance_);
}

void Device::setup_debug_messenger() {
    if (!ENABLE_VALIDATION_LAYERS) return;

    VkDebugUtilsMessengerCreateInfoEXT create_info;
    vku::default_debug_messenger_create_info(create_info);

    if (vku::CreateDebugUtilsMessengerEXT(instance_, &create_info, nullptr, &debug_messenger_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to set up Vulkan debug messenger!");
    }
}

void Device::create_surface()
{
    surface_ = window_->create_surface(instance_);
}

bool Device::check_device_extension_support(const VkPhysicalDevice& device) {
    uint32_t extension_count;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr);

    std::vector<VkExtensionProperties> available_extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, available_extensions.data());

    std::set<std::string> required_extensions(device_extensions.begin(), device_extensions.end());

    for (const auto& extension : available_extensions) {
        required_extensions.erase(extension.extensionName);
    }

    return required_extensions.empty();
}

bool Device::is_device_suitable(const VkPhysicalDevice& device) {
    QueueFamilyIndices indices = get_queue_families(device);

    bool extensions_supported = check_device_extension_support(device);

    bool swap_chain_adequate = false;
    if (extensions_supported) {
        SwapChainSupportDetails swapChainSupport = get_swap_chain_support(device);
        swap_chain_adequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
    }

    // Set up feature query
    VkPhysicalDeviceBufferDeviceAddressFeatures buffer_device_address_features{};
    buffer_device_address_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;

    VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering_features{};
    dynamic_rendering_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dynamic_rendering_features.pNext = &buffer_device_address_features;

    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT extended_dynamic_state2_features{};
    extended_dynamic_state2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
    extended_dynamic_state2_features.pNext = &dynamic_rendering_features;

    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT extended_dynamic_state3_features{};
    extended_dynamic_state3_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
    extended_dynamic_state3_features.pNext = &extended_dynamic_state2_features;

    VkPhysicalDeviceDescriptorIndexingFeatures descriptor_indexing_features{};
    descriptor_indexing_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    descriptor_indexing_features.pNext = &extended_dynamic_state3_features;


    VkPhysicalDeviceVulkan12Features vulkan12_features{};
    vulkan12_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12_features.pNext = &extended_dynamic_state2_features;
    VkPhysicalDeviceVulkan13Features vulkan13_features{};
    vulkan13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13_features.pNext = &vulkan12_features;

    VkPhysicalDeviceFeatures2 supported_features{};
    supported_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supported_features.pNext = &vulkan13_features;

    // Query what features are supported
    vkGetPhysicalDeviceFeatures2(device, &supported_features);

    // Check if desired features are supported
    bool has_desired_features = supported_features.features.samplerAnisotropy
        && vulkan13_features.dynamicRendering == VK_TRUE
        && vulkan13_features.synchronization2 == VK_TRUE
        && vulkan12_features.timelineSemaphore == VK_TRUE
        && vulkan12_features.bufferDeviceAddress == VK_TRUE
        && extended_dynamic_state2_features.extendedDynamicState2 == VK_TRUE;
        //&& extended_dynamic_state3_features.extendedDynamicState3 == VK_TRUE;

    return indices.can_render() && extensions_supported && swap_chain_adequate && has_desired_features;
}

void Device::select_physical_device()
{
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (device_count == 0) {
        throw std::runtime_error("Failed to find GPUs with Vulkan support!");
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());

    for (const auto& device : devices) {
        if (is_device_suitable(device)) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device, &props);
            // Some laptops, like one of my own, have both an integrated GPU and a discrete
            // one. It is not always the case that the discrete GPU is selected or prioritized
            // without this check.
            // TODO(DCut): Handle multiple discrete GPU, or utilize both discrete and integrated
            if (props.deviceType == VkPhysicalDeviceType::VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                physical_device_ = device;
                break;
            }
            // Fallback to integrated
            if (props.deviceType == VkPhysicalDeviceType::VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                physical_device_ = device;
                break;
            }
            // Fallback to use whatever device available
            physical_device_ = device;
        }
    }

    if (physical_device_ == VK_NULL_HANDLE) {
        throw std::runtime_error("Failed to find a suitable GPU!");
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device_, &props);
    STRING_LOG_INFO("Device name: {}", props.deviceName);
}

void Device::create_logical_device()
{
    QueueFamilyIndices indices = get_queue_families(physical_device_);

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies = {
        indices.graphics_family.value(),
        indices.compute_family.value(),
        indices.present_family.value()
    };

    float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies) {
        // clang-format off
        VkDeviceQueueCreateInfo queue_create_info = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queueFamilyIndex = queueFamily,
            .queueCount = 1,
            .pQueuePriorities = &queuePriority,
        };
        // clang-format on
        queueCreateInfos.push_back(queue_create_info);
    }

    // Only enable the features we want, and there are many:
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT extended_dynamic_state2_features{};
    extended_dynamic_state2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
    extended_dynamic_state2_features.extendedDynamicState2 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features vulkan12_features{};
    vulkan12_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12_features.pNext = &extended_dynamic_state2_features;
    vulkan12_features.timelineSemaphore = VK_TRUE;
    vulkan12_features.bufferDeviceAddress = VK_TRUE;

    VkPhysicalDeviceVulkan13Features vulkan13_features{};
    vulkan13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13_features.pNext = &vulkan12_features;
    vulkan13_features.synchronization2 = VK_TRUE;
    vulkan13_features.dynamicRendering = VK_TRUE;

    VkPhysicalDeviceFeatures enabled_device_features{};
    enabled_device_features.samplerAnisotropy = VK_TRUE;

    // clang-format off
    VkPhysicalDeviceFeatures2 enabled_device_features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vulkan13_features,
        .features = enabled_device_features
    };
    // clang-format on

    // clang-format off
    VkDeviceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enabled_device_features2,
        .flags = 0,
        .queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size()),
        .pQueueCreateInfos = queueCreateInfos.data(),
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = static_cast<uint32_t>(device_extensions.size()),
        .ppEnabledExtensionNames = device_extensions.data(),
        .pEnabledFeatures = nullptr // Data is passed through pNext instead
    };
    // clang-format on

    if (ENABLE_VALIDATION_LAYERS) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(validation_layers.size());
        createInfo.ppEnabledLayerNames = validation_layers.data();
    }

    if (vkCreateDevice(physical_device_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
        throw std::runtime_error("failed to create logical device!");
    }

    volkLoadDevice(device_);
}

SwapChainSupportDetails Device::get_swap_chain_support(const VkPhysicalDevice& physical_device)
{
    SwapChainSupportDetails details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface_, &details.capabilities);

    uint32_t format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface_, &format_count, nullptr);

    if (format_count != 0) {
        details.formats.resize(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface_, &format_count, details.formats.data());
    }

    uint32_t present_mode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface_, &present_mode_count, nullptr);

    if (present_mode_count != 0) {
        details.presentModes.resize(present_mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface_, &present_mode_count, details.presentModes.data());
    }

    return details;
}

SwapChainSupportDetails Device::get_swap_chain_support()
{
    return get_swap_chain_support(physical_device_);
}

QueueFamilyIndices Device::get_queue_families(const VkPhysicalDevice& physical_device)
{
    QueueFamilyIndices indices;

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queueFamilies.data());

    // Track best candidate for transfer queue
    std::optional<uint32_t> dedicated_transfer;

    for (uint32_t i = 0; i < queue_family_count; i++)
    {
        const auto& queueFamily = queueFamilies[i];

        if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            indices.graphics_family = i;
        }
        if ((queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            !(queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT))
        {
            indices.compute_family = i;
        }

        // Check if this is a dedicated transfer queue
        if ((queueFamily.queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            !(queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            !(queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT))
        {
            indices.transfer_family = i;
        }

        // Present support
        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, i, surface_, &presentSupport);
        if (presentSupport) {
            indices.present_family = i;
        }
    }

    return indices;
}

QueueFamilyIndices Device::get_queue_families()
{
    return get_queue_families(physical_device_);
}

VkFormat Device::get_format_support(const std::vector<VkFormat>& candidates, const VkImageTiling& tiling, const VkFormatFeatureFlags& features)
{
    for (VkFormat format : candidates)
    {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physical_device_, format, &props);

        if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features)
            return format;
        else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features)
            return format;
    }

    throw std::runtime_error("Failed to find supported format!");
}

uint32_t Device::get_memory_type(const uint32_t& type_filter, const VkMemoryPropertyFlags& properties)
{
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);

    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) && (memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("failed to find suitable memory type!");
}

VkPhysicalDeviceLimits Device::get_physical_device_limits()
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device_, &properties);

    return properties.limits;
}

VkFormat Device::get_depth_format() {
    // clang-format off
    return get_format_support(
        {
            VK_FORMAT_D32_SFLOAT,
            VK_FORMAT_D32_SFLOAT_S8_UINT,
            VK_FORMAT_D24_UNORM_S8_UINT
        },
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    // clang-format on
}

auto Device::create_command_recorder(const QueueType& queue_type) -> std::unique_ptr<CommandRecorder>
{
    // return std::move(std::make_unique<CommandRecorder>(device_, get_queue_families(), queue_type));
}

VkSurfaceKHR Device::get_surface() const
{
    return surface_;
}

VkDevice& Device::get_device()
{
    return device_;
}

auto Device::get_queue(const QueueType& type) -> Queue
{
    QueueFamilyIndices indices = get_queue_families();

    Queue queue = {
        .queue_family_index = 0,
        .type = type,
        .queue = VK_NULL_HANDLE,
    };

    switch (type)
    {
        case QueueType::GRAPHICS:
        {
            if (indices.graphics_family.has_value())
            {
                STRING_LOG_DEBUG("Creating graphics queue.");
                queue.queue_family_index = indices.graphics_family.value();
                vkGetDeviceQueue(device_, queue.queue_family_index, 0, &queue.queue);
                return std::move(queue);
            }
            break;
        }
        case QueueType::COMPUTE:
        {
            if (indices.compute_family.has_value())
            {
                STRING_LOG_DEBUG("Creating compute queue.");
                queue.queue_family_index = indices.compute_family.value();
                vkGetDeviceQueue(device_, queue.queue_family_index, 0, &queue.queue);
                return std::move(queue);
            }
            break;
        }
        case QueueType::TRANSFER:
        {
            if (indices.transfer_family.has_value())
            {
                STRING_LOG_DEBUG("Creating transfer queue.");
                queue.queue_family_index = indices.transfer_family.value();
                vkGetDeviceQueue(device_, queue.queue_family_index, 0, &queue.queue);
                return std::move(queue);
            }
            break;
        }
        case QueueType::PRESENT:
        {
            if (indices.present_family.has_value())
            {
                STRING_LOG_DEBUG("Creating present queue.");
                queue.queue_family_index = indices.present_family.value();
                vkGetDeviceQueue(device_, queue.queue_family_index, 0, &queue.queue);
                return std::move(queue);
            }
            break;
        }
        default:
            throw std::runtime_error("Attempt to get queue type which is not supported!");   
    }

    throw std::runtime_error("Failed to get queue!");
}

Allocator& Device::get_allocator() const
{
    return *allocator_.get();
}

#include <string.h>

bool Device::are_validation_layer_supported()
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