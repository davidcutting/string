#pragma once

#include <string_view>
#include <string/core/logger.hpp>
#include <string/vulkan/render_data.hpp>
#include <string/gpu/resource.hpp>
#include <vector>
#include <fstream>
#include <filesystem>
#include "string/gpu/command_recorder.hpp"

#include <volk.h>

namespace String
{

namespace vku
{

// A single image layout transition recorded into an already-open command buffer, using
// synchronization2 (VkImageMemoryBarrier2). Unlike transition_image_layout (which self-submits
// a one-shot upload barrier and derives its scopes from the layout pair), this just records
// into `command_buffer` with caller-supplied src/dst scopes — the barrier primitive the frame
// loop (and, later, the render graph) builds on.
struct ImageTransition
{
    VkImage image;
    VkImageLayout old_layout;
    VkImageLayout new_layout;
    VkPipelineStageFlags2 src_stage;
    VkAccessFlags2 src_access;
    VkPipelineStageFlags2 dst_stage;
    VkAccessFlags2 dst_access;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    // Mip range to transition; defaults to just the base level (mip generation transitions
    // individual levels as it blits down the chain).
    uint32_t base_mip = 0;
    uint32_t level_count = 1;
};

inline void transition_image(VkCommandBuffer command_buffer, const ImageTransition& t)
{
    const VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = t.src_stage,
        .srcAccessMask = t.src_access,
        .dstStageMask = t.dst_stage,
        .dstAccessMask = t.dst_access,
        .oldLayout = t.old_layout,
        .newLayout = t.new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = t.image,
        .subresourceRange = { t.aspect, t.base_mip, t.level_count, 0, 1 },
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 0,
        .pMemoryBarriers = nullptr,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

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

inline bool hasStencilComponent(VkFormat format)
{
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
}

}  // namespace vku
}  // namespace String
