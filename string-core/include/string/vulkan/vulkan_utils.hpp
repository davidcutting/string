#pragma once

#include <string_view>
#include <string/core/logger.hpp>
#include <string/vulkan/render_data.hpp>
#include <string/gpu/resource.hpp>
#include <vector>
#include <fstream>
#include <filesystem>

#include <volk.h>

namespace string
{

namespace vku
{

inline std::vector<char> read_file(const std::filesystem::path& filepath)
{
    std::ifstream file(filepath, std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filepath.string());
    }

    size_t fileSize = (size_t)file.tellg();
    std::vector<char> buffer(fileSize);

    file.seekg(0);
    file.read(buffer.data(), fileSize);

    file.close();

    return buffer;
}

inline VkShaderModule create_shader_module(VkDevice device, const std::vector<char>& code)
{
    VkShaderModuleCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = code.size(),
        .pCode = reinterpret_cast<const uint32_t*>(code.data())
    };

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &create_info, nullptr, &shaderModule) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create shader module!");
    }

    return shaderModule;
}

inline VkShaderModule load_shader_from_disk(VkDevice device, const std::filesystem::path& filepath)
{
    const auto binary = read_file(filepath);
    return create_shader_module(device, binary);
}

// Create a shader module from SPIR-V words already in memory (the in-process Slang compile path,
// vs. load_shader_from_disk's precompiled .spv files).
inline VkShaderModule create_shader_module_spirv(VkDevice device, const std::vector<uint32_t>& code)
{
    VkShaderModuleCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = code.size() * sizeof(uint32_t),
        .pCode = code.data()
    };

    VkShaderModule shader_module;
    if (vkCreateShaderModule(device, &create_info, nullptr, &shader_module) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create shader module!");
    }

    return shader_module;
}

inline VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
                                             const VkAllocationCallbacks* pAllocator,
                                             VkDebugUtilsMessengerEXT* pDebugMessenger) {
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    } else {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

void load_model(const std::filesystem::path& model_path, std::vector<Vertex>& vertex_buffer, std::vector<uint32_t>& index_buffer);

inline void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger,
                                          const VkAllocationCallbacks* pAllocator) {
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func != nullptr) {
        func(instance, debugMessenger, pAllocator);
    }
}

inline VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                                    VkDebugUtilsMessageTypeFlagsEXT /*messageType*/,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                    void* /*pUserData*/) {
    const std::string_view message{pCallbackData->pMessage};
    if (messageSeverity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        STRING_LOG_TRACE("VK Validation: {}", message);
    } else if (messageSeverity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        STRING_LOG_INFO("VK Validation: {}", message);
    } else if (messageSeverity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        STRING_LOG_WARN("VK Validation: {}", message);
    } else if (messageSeverity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        STRING_LOG_ERROR("VK Validation Layer: {}", message);
    } else {
        STRING_LOG_CRITICAL("VK Validation Layer: {}", message);
    }
    return VK_FALSE;
}

inline void default_debug_messenger_create_info(VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
    // clang-format off
    createInfo = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        // VERBOSE is deliberately omitted: it's loader/layer diagnostic chatter (ICD probing,
        // "Loading layer library", the Mesa device_select layer's "Copying old device ...")
        // — not actionable. Re-add VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT here for
        // deep loader/layer debugging. WARNING + ERROR keep every actionable validation message.
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_callback,
        .pUserData = nullptr
    };
    // clang-format on
}

}  // namespace vku
}  // namespace string
