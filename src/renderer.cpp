#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string/renderer.hpp>
#include <string/vulkan_utils.hpp>
#include <string/allocator.hpp>
#include <string/device.hpp>
#include <string/pipelines/pipeline_2d.hpp>
#include <string/render_data.hpp>
#include "glm/fwd.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <string/core/stb_image.h>

#include <stdexcept>

namespace String {

void Renderer::createTextureImage() {
    int width, height, channels;
    stbi_uc* pixels = stbi_load(TEXTURE_PATH.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    VkDeviceSize image_size = width * height * 4;

    if (!pixels) {
        throw std::runtime_error("failed to load texture image!");
    }

    // Allocate staging buffer and copy stbi image into staging buffer
    auto staging_buffer = device_->get_allocator().create_staging_buffer(image_size);
    device_->get_allocator().copy_data_to_buffer(pixels, staging_buffer.get());

    // De-allocate stbi image
    stbi_image_free(pixels);

    const auto image_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    texture_image_ = device_->get_allocator().create_image(width, height, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL, image_usage, VMA_MEMORY_USAGE_GPU_ONLY);

    transitionImageLayout(texture_image_->image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(staging_buffer->buffer, texture_image_->image, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    transitionImageLayout(texture_image_->image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Destroy staging buffer
    device_->get_allocator().destroy_buffer(staging_buffer);
}

void Renderer::cleanup() {
    vkDestroyImageView(device_->get_device(), depthImageView, nullptr);
    device_->get_allocator().destroy_image(depth_image_);

    // TODO(DCut): Why does .release() have bugprone ignored return?
    swap_chain_.reset();

    pipeline_3d_.reset();

    for (size_t i = 0; i < swap_chain_image_count_; i++)
    {
        vmaUnmapMemory(device_->get_allocator().get_allocator(), camera_3d_ubo_[i]->allocation);
        device_->get_allocator().destroy_buffer(camera_3d_ubo_[i]);

        vmaUnmapMemory(device_->get_allocator().get_allocator(), camera_2d_ubo_[i]->allocation);
        device_->get_allocator().destroy_buffer(camera_2d_ubo_[i]);

        vmaUnmapMemory(device_->get_allocator().get_allocator(), ui_shapes_ssbo_[i]->allocation);
        device_->get_allocator().destroy_buffer(ui_shapes_ssbo_[i]);
    }

    vkDestroyDescriptorPool(device_->get_device(), descriptorPool, nullptr);

    vkDestroySampler(device_->get_device(), textureSampler, nullptr);
    vkDestroyImageView(device_->get_device(), textureImageView, nullptr);

    device_->get_allocator().destroy_image(texture_image_);

    vkDestroyDescriptorSetLayout(device_->get_device(), descriptor_set_layout_3d_, nullptr);
    vkDestroyDescriptorSetLayout(device_->get_device(), ui_descriptor_set_layout_, nullptr);

    device_->get_allocator().destroy_buffer(scene_3d_.index_buffer);
    device_->get_allocator().destroy_buffer(scene_3d_.vertex_buffer);

    for (size_t i = 0; i < swap_chain_image_count_; ++i)
    {
        vkDestroySemaphore(device_->get_device(), render_complete_semaphores_[i], nullptr);
        vkDestroySemaphore(device_->get_device(), image_available_semaphores_[i], nullptr);
        vkDestroyFence(device_->get_device(), frame_in_flight_fences_[i], nullptr);
    }

    vkDestroyCommandPool(device_->get_device(), commandPool, nullptr);

    device_.reset();
}

void Renderer::recreateSwapChain() {
    int width = 0, height = 0;
    glfwGetFramebufferSize(window_->get_native_handle(), &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(window_->get_native_handle(), &width, &height);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(device_->get_device());

    swap_chain_.reset();
    swap_chain_ = std::make_unique<Swapchain>(device_, VkExtent2D{
        .width = static_cast<uint32_t>(width),
        .height = static_cast<uint32_t>(height)
    });

    // Clean up depth resources and re-allocate
    vkDestroyImageView(device_->get_device(), depthImageView, nullptr);

    device_->get_allocator().destroy_image(depth_image_);
    createDepthResources();
}

void Renderer::createDescriptorSetLayout()
{
    {
        // clang-format off
        VkDescriptorSetLayoutBinding ubo_layout_binding = {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .pImmutableSamplers = nullptr
        };

        VkDescriptorSetLayoutBinding sampler_layout_binding = {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = nullptr
        };
        // clang-format on

        std::vector<VkDescriptorSetLayoutBinding> bindings = {ubo_layout_binding, sampler_layout_binding};

        // clang-format off
        VkDescriptorSetLayoutCreateInfo layout_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .bindingCount = static_cast<uint32_t>(bindings.size()),
            .pBindings = bindings.data()
        };
        // clang-format on

        if (vkCreateDescriptorSetLayout(device_->get_device(), &layout_info, nullptr, &descriptor_set_layout_3d_) != VK_SUCCESS) {
            throw std::runtime_error("failed to create descriptor set layout!");
        }
    }

    {
        // clang-format off
        VkDescriptorSetLayoutBinding ubo_layout_binding = {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = nullptr
        };

        VkDescriptorSetLayoutBinding ssbo_layout_binding = {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = nullptr
        };
        // clang-format on

        std::vector<VkDescriptorSetLayoutBinding> bindings = {ubo_layout_binding, ssbo_layout_binding};

        // clang-format off
        VkDescriptorSetLayoutCreateInfo layout_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .bindingCount = static_cast<uint32_t>(bindings.size()),
            .pBindings = bindings.data()
        };
        // clang-format on

        if (vkCreateDescriptorSetLayout(device_->get_device(), &layout_info, nullptr, &ui_descriptor_set_layout_) != VK_SUCCESS) {
            throw std::runtime_error("failed to create descriptor set layout!");
        }
    }
}

void Renderer::createCommandPool() {
    QueueFamilyIndices queueFamilyIndices = device_->get_queue_families();

    // clang-format off
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queueFamilyIndices.graphics_family.value()
    };
    // clang-format on

    if (vkCreateCommandPool(device_->get_device(), &pool_info, nullptr, &commandPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create graphics command pool!");
    }
}

void Renderer::createDepthResources() {
    VkFormat depth_format = device_->get_depth_format();
    VkExtent2D extent = swap_chain_->get_extent();

    depth_image_ = device_->get_allocator().create_image(extent.width, extent.height, depth_format,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

    depthImageView = createImageView(depth_image_->image, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);
}

bool Renderer::hasStencilComponent(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
}

void Renderer::createTextureImageView() {
    textureImageView = createImageView(texture_image_->image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT);
}

void Renderer::createTextureSampler() {
    const auto physical_device_limits = device_->get_physical_device_limits();

    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .mipLodBias = 0.f,
        .anisotropyEnable = VK_TRUE,
        .maxAnisotropy = physical_device_limits.maxSamplerAnisotropy,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = 0.f,
        .maxLod = 0.f,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE
    };

    if (vkCreateSampler(device_->get_device(), &sampler_info, nullptr, &textureSampler) != VK_SUCCESS) {
        throw std::runtime_error("failed to create texture sampler!");
    }
}

VkImageView Renderer::createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags) {
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = {},
        .subresourceRange = {
            .aspectMask = aspectFlags,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };

    VkImageView image_view;
    if (vkCreateImageView(device_->get_device(), &view_info, nullptr, &image_view) != VK_SUCCESS) {
        throw std::runtime_error("failed to create image view!");
    }

    return image_view;
}

void Renderer::createImage(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
                 VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& imageMemory) {
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {
            .width = width,
            .height = height,
            .depth = 1
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = tiling,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    if (vkCreateImage(device_->get_device(), &image_info, nullptr, &image) != VK_SUCCESS) {
        throw std::runtime_error("failed to create image!");
    }

    VkMemoryRequirements memory_requirements;
    vkGetImageMemoryRequirements(device_->get_device(), image, &memory_requirements);

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .allocationSize = memory_requirements.size,
        .memoryTypeIndex = device_->get_memory_type(memory_requirements.memoryTypeBits, properties)
    };

    if (vkAllocateMemory(device_->get_device(), &alloc_info, nullptr, &imageMemory) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate image memory!");
    }

    vkBindImageMemory(device_->get_device(), image, imageMemory, 0);
}

void Renderer::transitionImageLayout(VkImage image, VkFormat /*format*/, VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = 0,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        throw std::invalid_argument("unsupported layout transition!");
    }

    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    endSingleTimeCommands(commandBuffer);
}

void Renderer::copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1}
    };

    vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    endSingleTimeCommands(commandBuffer);
}

void Renderer::create_vertex_buffer()
{
    if (vertices.empty()) {
        throw std::runtime_error("Cannot create vertex buffer: empty vertices vector");
    }

    VkDeviceSize buffer_size = sizeof(vertices[0]) * vertices.size();
    const auto vertex_buffer_usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    // Allocate Buffers
    auto staging_buffer = device_->get_allocator().create_staging_buffer(buffer_size);
    scene_3d_.vertex_buffer = device_->get_allocator().create_buffer(buffer_size, vertex_buffer_usage, VMA_MEMORY_USAGE_GPU_ONLY);

    // Copy vertices vector into staging buffer
    device_->get_allocator().copy_data_to_buffer(vertices.data(), staging_buffer.get());

    // Submit commands to copy staging buffer to GPU memory
    copy_buffer(staging_buffer.get(), scene_3d_.vertex_buffer.get());

    // Destroy staging buffer
    device_->get_allocator().destroy_buffer(staging_buffer);
}

void Renderer::create_index_buffer()
{
    if (indices.empty()) {
        throw std::runtime_error("Cannot create index buffer: empty indices vector");
    }

    const VkDeviceSize buffer_size = sizeof(indices[0]) * indices.size();
    const auto index_buffer_usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

    // Allocate Buffers
    auto staging_buffer = device_->get_allocator().create_staging_buffer(buffer_size);
    scene_3d_.index_buffer = device_->get_allocator().create_buffer(buffer_size, index_buffer_usage, VMA_MEMORY_USAGE_GPU_ONLY);

    // Copy indices vector into staging buffer
    device_->get_allocator().copy_data_to_buffer(indices.data(), staging_buffer.get());

    // Submit commands to copy staging buffer to GPU memory
    copy_buffer(staging_buffer.get(), scene_3d_.index_buffer.get());

    // Destroy staging buffer
    device_->get_allocator().destroy_buffer(staging_buffer);
}

void Renderer::createUniformBuffers()
{
    {
        VkDeviceSize buffer_size = sizeof(Camera3D);

        camera_3d_ubo_.resize(swap_chain_image_count_);
        uniform_buffers_mapped_.resize(swap_chain_image_count_);

        for (size_t i = 0; i < swap_chain_image_count_; i++)
        {
            camera_3d_ubo_[i] = device_->get_allocator().create_buffer(buffer_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
            vmaMapMemory(device_->get_allocator().get_allocator(), camera_3d_ubo_[i]->allocation, &uniform_buffers_mapped_[i]);
        }
    }
    
    {
        VkDeviceSize buffer_size = sizeof(Camera2D);

        camera_2d_ubo_.resize(swap_chain_image_count_);
        camera_2d_mapped_.resize(swap_chain_image_count_);

        for (size_t i = 0; i < swap_chain_image_count_; i++)
        {
            camera_2d_ubo_[i] = device_->get_allocator().create_buffer(buffer_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
            vmaMapMemory(device_->get_allocator().get_allocator(), camera_2d_ubo_[i]->allocation, &camera_2d_mapped_[i]);
        }
    }
}

void Renderer::create_ssbo_buffer()
{
    VkDeviceSize buffer_size = sizeof(UIShape) * 2;

    ui_shapes_ssbo_.resize(swap_chain_image_count_);
    ui_elements_mapped_.resize(swap_chain_image_count_);

    for (size_t i = 0; i < swap_chain_image_count_; i++)
    {
        ui_shapes_ssbo_[i] = device_->get_allocator().create_buffer(buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        vmaMapMemory(device_->get_allocator().get_allocator(), ui_shapes_ssbo_[i]->allocation, &ui_elements_mapped_[i]);
    }
}

void Renderer::createDescriptorPool()
{
    std::vector<VkDescriptorPoolSize> pool_sizes = {
        {
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = static_cast<uint32_t>(swap_chain_image_count_)
        },
        {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = static_cast<uint32_t>(swap_chain_image_count_)
        },
        {
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = static_cast<uint32_t>(swap_chain_image_count_)
        },
        {
            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = static_cast<uint32_t>(swap_chain_image_count_)
        },
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .maxSets = static_cast<uint32_t>(swap_chain_image_count_) * 2,
        .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    };

    if (vkCreateDescriptorPool(device_->get_device(), &pool_info, nullptr, &descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create descriptor pool!");
    }
}

void Renderer::createDescriptorSets()
{
    // 3D Scene Descriptor Sets
    std::vector<VkDescriptorSetLayout> layouts_3d(swap_chain_image_count_, descriptor_set_layout_3d_);
    VkDescriptorSetAllocateInfo descriptor_set_3d_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = descriptorPool,
        .descriptorSetCount = static_cast<uint32_t>(swap_chain_image_count_),
        .pSetLayouts = layouts_3d.data()
    };
    descriptor_sets_3d_.resize(swap_chain_image_count_);

    if (vkAllocateDescriptorSets(device_->get_device(), &descriptor_set_3d_alloc_info, descriptor_sets_3d_.data()) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to allocate 3D descriptor sets!");
    }

    for (size_t i = 0; i < swap_chain_image_count_; i++) {
        VkDescriptorBufferInfo buffer_info = {
            .buffer = camera_3d_ubo_[i]->buffer,
            .offset = 0,
            .range = camera_3d_ubo_[i]->buffer_size
        };

        VkDescriptorImageInfo image_info = {
            .sampler = textureSampler,
            .imageView = textureImageView,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };

        std::vector<VkWriteDescriptorSet> descriptor_writes = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = descriptor_sets_3d_[i],
                .dstBinding = 0,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pImageInfo = nullptr,
                .pBufferInfo = &buffer_info,
                .pTexelBufferView = nullptr,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = descriptor_sets_3d_[i],
                .dstBinding = 1,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &image_info,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            }
        };

        vkUpdateDescriptorSets(
            device_->get_device(),
            static_cast<uint32_t>(descriptor_writes.size()),
            descriptor_writes.data(), 0,
            nullptr);
    }

    // UI Scene Descriptor Sets
    std::vector<VkDescriptorSetLayout> ui_layouts(swap_chain_image_count_, ui_descriptor_set_layout_);
    VkDescriptorSetAllocateInfo ui_descriptor_set_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = descriptorPool,
        .descriptorSetCount = static_cast<uint32_t>(swap_chain_image_count_),
        .pSetLayouts = ui_layouts.data()
    };
    ui_descriptor_sets_.resize(swap_chain_image_count_);

    if (vkAllocateDescriptorSets(device_->get_device(), &ui_descriptor_set_alloc_info, ui_descriptor_sets_.data()) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to allocate UI descriptor sets!");
    }

    for (size_t i = 0; i < swap_chain_image_count_; i++) {
        VkDescriptorBufferInfo camera_2d_ubo_info = {
            .buffer = camera_2d_ubo_[i]->buffer,
            .offset = 0,
            .range = camera_2d_ubo_[i]->buffer_size
        };

        VkDescriptorBufferInfo ui_shapes_ssbo_info = {
            .buffer = ui_shapes_ssbo_[i]->buffer,
            .offset = 0,
            .range = ui_shapes_ssbo_[i]->buffer_size
        };

        std::vector<VkWriteDescriptorSet> descriptor_writes = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = ui_descriptor_sets_[i],
                .dstBinding = 0,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pImageInfo = nullptr,
                .pBufferInfo = &camera_2d_ubo_info,
                .pTexelBufferView = nullptr,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = ui_descriptor_sets_[i],
                .dstBinding = 1,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pImageInfo = nullptr,
                .pBufferInfo = &ui_shapes_ssbo_info,
                .pTexelBufferView = nullptr,
            }
        };

        vkUpdateDescriptorSets(
            device_->get_device(),
            static_cast<uint32_t>(descriptor_writes.size()),
            descriptor_writes.data(), 0,
            nullptr);
    }
}

VkCommandBuffer Renderer::beginSingleTimeCommands() {
    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };

    VkCommandBuffer command_buffer;
    vkAllocateCommandBuffers(device_->get_device(), &alloc_info, &command_buffer);

    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr
    };

    vkBeginCommandBuffer(command_buffer, &beginInfo);

    return command_buffer;
}

void Renderer::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr
    };

    vkQueueSubmit(device_->get_graphics_queue(), 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(device_->get_graphics_queue());

    vkFreeCommandBuffers(device_->get_device(), commandPool, 1, &commandBuffer);
}

void Renderer::copy_buffer(const Buffer* src, const Buffer* dest) {
    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    VkBufferCopy copy_region = {
        .srcOffset = 0,
        .dstOffset = 0,
        .size = dest->buffer_size
    };

    vkCmdCopyBuffer(commandBuffer, src->buffer, dest->buffer, 1, &copy_region);

    endSingleTimeCommands(commandBuffer);
}

void Renderer::createCommandBuffers() {
    commandBuffers.resize(swap_chain_image_count_);

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = (uint32_t)commandBuffers.size()
    };

    if (vkAllocateCommandBuffers(device_->get_device(), &alloc_info, commandBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate command buffers!");
    }
}

void Renderer::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    // TODO(DCut): Refactor begin and end command buffer function because it is doing
    // an additional allocation we may not always need
    // Also, it isn't used here because those allocations can be reused and its a lot
    // for one frame
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("failed to begin recording command buffer!");
    }

    // TODO(DCut): Clean up image transition function and refactor this to use it
    // Transition swapchain image from UNDEFINED to COLOR_ATTACHMENT_OPTIMAL
    VkImageMemoryBarrier image_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = swap_chain_->get_images()[imageIndex],
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        }
    };

    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &image_barrier);

    VkRenderingAttachmentInfo color_attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = swap_chain_->get_image_views()[imageIndex],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {
            .color = {{ 0.0f, 0.0f, 0.0f, 0.0f }}
        }
    };

    VkRenderingAttachmentInfo depth_attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = depthImageView,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue = {
            .depthStencil = {1.0f, 0}
        }
    };

    const auto swap_chain_extent = swap_chain_->get_extent();

    VkRenderingInfo rendering_info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderArea = {{0 , 0}, swap_chain_extent},
        .layerCount = 1,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment,
        .pDepthAttachment = &depth_attachment,
        .pStencilAttachment = nullptr,
    };

    vkCmdBeginRendering(commandBuffer, &rendering_info);

    /// -----------------------------------------------------------------------------------------

    // Dynamic State

    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(swap_chain_extent.width),
        .height = static_cast<float>(swap_chain_extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = swap_chain_extent
    };

    /// -----------------------------------------------------------------------------------------

    // Clear screen

    // const VkClearColorValue light_gray = {{ 0.557f, 0.557f, 0.576f, 1.0f }};
    // const VkClearColorValue gray = {{ 0.388f, 0.388f, 0.4f, 1.0f }};
    // const VkClearColorValue dark_gray = {{ 0.173, 0.173, 0.18, 1.0f }};
    const VkClearColorValue darker_gray = {{ 0.11, 0.11, 0.118, 1.0f }};


    VkClearAttachment clear_attachment = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .colorAttachment = 0,
        .clearValue = {
            .color = darker_gray
        },
    };


    VkClearRect clear_rect = {
        .rect = {
            .offset = {0, 0},
            .extent = swap_chain_extent,
        },
        .baseArrayLayer = 0,
        .layerCount = 1
    };

    vkCmdClearAttachments(commandBuffer, 1, &clear_attachment, 1, &clear_rect);

    /// -----------------------------------------------------------------------------------------

    // Draw 2D Grid

    // vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_grid_2d_->get_pipeline());
    // vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    // vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    // vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_grid_2d_->get_pipeline_layout(),
    //     0, 1, &grid_2d_descriptor_sets_[current_frame], 0, nullptr);
    
    // vkCmdPushConstants(commandBuffer, pipeline_grid_2d_->get_pipeline_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
    //     0, sizeof(ui_push_constant_), &ui_push_constant_);
    // vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    /// -----------------------------------------------------------------------------------------

    // Draw 3D

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_3d_->get_pipeline());
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    VkBuffer vertex_buffers_3d[] = { scene_3d_.vertex_buffer->buffer };
    VkDeviceSize vertex_buffer_3d_offsets[] = { 0 };
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertex_buffers_3d, vertex_buffer_3d_offsets);
    vkCmdBindIndexBuffer(commandBuffer, scene_3d_.index_buffer->buffer, 0, VK_INDEX_TYPE_UINT32);

    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_3d_->get_pipeline_layout(), 0, 1,
                            &descriptor_sets_3d_[current_frame], 0, nullptr);

    vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);

    /// -----------------------------------------------------------------------------------------

    // Draw UI

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_->get_pipeline());
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_->get_pipeline_layout(),
        0, 1, &ui_descriptor_sets_[current_frame], 0, nullptr);
    
    vkCmdPushConstants(commandBuffer, ui_pipeline_->get_pipeline_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(ui_push_constant_), &ui_push_constant_);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    /// -----------------------------------------------------------------------------------------

    vkCmdEndRendering(commandBuffer);

    // Transition swapchain image from COLOR_ATTACHMENT_OPTIMAL to PRESENT_SRC_KHR
    image_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image_barrier.dstAccessMask = 0;
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &image_barrier);
    
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("failed to record command buffer!");
    }
}

void Renderer::createSyncObjects()
{
    image_available_semaphores_.resize(swap_chain_image_count_);
    render_complete_semaphores_.resize(swap_chain_image_count_);
    frame_in_flight_fences_.resize(swap_chain_image_count_);

    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0
    };

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };

    for (size_t i = 0; i < swap_chain_image_count_; i++)
    {
        if (vkCreateSemaphore(device_->get_device(), &semaphore_info, nullptr, &image_available_semaphores_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device_->get_device(), &semaphore_info, nullptr, &render_complete_semaphores_[i]) != VK_SUCCESS ||
            vkCreateFence(device_->get_device(), &fence_info, nullptr, &frame_in_flight_fences_[i]) != VK_SUCCESS) 
        {
            throw std::runtime_error("Failed to create synchronization objects for a frame!");
        }
    }
}

void Renderer::updateUniformBuffer(uint32_t currentImage) {
    static auto startTime = std::chrono::high_resolution_clock::now();

    auto currentTime = std::chrono::high_resolution_clock::now();
    float time = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();

    const auto swap_chain_extent = swap_chain_->get_extent();

    {
        Camera3D camera = {
            .model = glm::rotate(glm::mat4(1.0f), time * glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
            .view = glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
            .proj = glm::perspective(glm::radians(45.0f), swap_chain_extent.width / (float)swap_chain_extent.height, 0.1f, 10.0f)
        };

        camera.proj[1][1] *= -1;

        memcpy(uniform_buffers_mapped_[currentImage], &camera, sizeof(camera));
    }
    
    {
        ui_push_constant_ = {
            .screen_size = { swap_chain_extent.width, swap_chain_extent.height },
            .num_shapes = 2,
            .delta_time = time
        };

        std::vector<UIElement> elements = {
            { // Giant blue circle
                .fill = {0.0f, 0.0f, 1.0f, 1.0f},
                .stroke = {0.0f, 1.0f, 0.0f, 1.0f},
                .position = {400, 400},
                .radius = 100.0f,
                .stroke_width = 10.0f,
            },
            { // Little red circle
                .fill = {1.0f, 0.0f, 0.0f, 1.0f},
                .stroke = {0.0f, 1.0f, 0.0f, 1.0f},
                .position = {100, 100},
                .radius = 50.0f,
                .stroke_width = 5.0f,
            }
        };

        memcpy(ui_elements_mapped_[currentImage], elements.data(), sizeof(UIElement) * elements.size());
    }
}

void Renderer::drawFrame() {
    vkWaitForFences(device_->get_device(), 1, &frame_in_flight_fences_[current_frame], VK_TRUE, UINT64_MAX);

    auto[result, image_index] = swap_chain_->acquire_next_frame(image_available_semaphores_[current_frame]);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapChain();
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("failed to acquire swap chain image!");
    }

    updateUniformBuffer(current_frame);
    vkResetFences(device_->get_device(), 1, &frame_in_flight_fences_[current_frame]);

    vkResetCommandBuffer(commandBuffers[current_frame], 0);
    recordCommandBuffer(commandBuffers[current_frame], image_index);

    const VkSemaphoreSubmitInfo wait_semaphore_infos[] = {
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = image_available_semaphores_[current_frame],
            .value = 0,
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .deviceIndex = 0
        }
    };

    const VkSemaphoreSubmitInfo signal_semaphore_infos[] = {
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = render_complete_semaphores_[current_frame],
            .value = 0,
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .deviceIndex = 0
        }
    };

    VkCommandBufferSubmitInfo command_buffer_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext = nullptr,
        .commandBuffer = commandBuffers[current_frame],
        .deviceMask = 0
    };

    VkSubmitInfo2 submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext = nullptr,
        .flags = 0,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = wait_semaphore_infos,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &command_buffer_info,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = signal_semaphore_infos
    };

    if (vkQueueSubmit2(device_->get_graphics_queue(), 1, &submit_info, frame_in_flight_fences_[current_frame]) != VK_SUCCESS) {
        throw std::runtime_error("failed to submit draw command buffer!");
    }

    VkSwapchainKHR swap_chains[] = { swap_chain_->get_swap_chain() };

    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &render_complete_semaphores_[current_frame],
        .swapchainCount = 1,
        .pSwapchains = swap_chains,
        .pImageIndices = &image_index,
        .pResults = nullptr
    };

    result = vkQueuePresentKHR(device_->get_present_queue(), &present_info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized) {
        framebufferResized = false;
        recreateSwapChain();
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("failed to present swap chain image!");
    }

    current_frame = (current_frame + 1) % swap_chain_image_count_;
}

}  // namespace String
