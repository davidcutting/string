#pragma once

#include <vulkan/vulkan.h>

void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer,
                  VkDeviceMemory& bufferMemory);

void destroy_buffer(VkBuffer& buffer);