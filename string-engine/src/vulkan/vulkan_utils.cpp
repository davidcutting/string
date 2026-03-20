#include <string>
#include <unordered_map>
#include <string/vulkan/render_data.hpp>
#include <string/vulkan/vulkan_utils.hpp>

#define TINYOBJLOADER_IMPLEMENTATION
#include <string/core/tiny_obj_loader.h>

namespace String
{
namespace vku
{

void transition_image_layout(CommandRecorder& transfer_command_recorder,
    VkImage image, VkFormat /*format*/, VkImageLayout old_layout, VkImageLayout new_layout)
{
    transfer_command_recorder.begin();
    VkCommandBuffer& command_buffer = transfer_command_recorder.get_command_buffer();

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = 0,
        .oldLayout = old_layout,
        .newLayout = new_layout,
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

    VkPipelineStageFlags source_stage;
    VkPipelineStageFlags destination_stage;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else
    {
        throw std::invalid_argument("unsupported layout transition!");
    }

    vkCmdPipelineBarrier(command_buffer, source_stage, destination_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    transfer_command_recorder.end();
    transfer_command_recorder.immediate_submit();
    transfer_command_recorder.reset();
}

void copy_buffer_to_image(
    CommandRecorder& transfer_command_recorder,
    const AllocatedBuffer& buffer,
    const AllocatedImage& image,
    const VkExtent2D& extent)
{
    transfer_command_recorder.begin();
    VkCommandBuffer& command_buffer = transfer_command_recorder.get_command_buffer();

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
        .imageExtent = {extent.width, extent.height, 1}
    };

    vkCmdCopyBufferToImage(command_buffer, buffer.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transfer_command_recorder.end();
    transfer_command_recorder.immediate_submit();
    transfer_command_recorder.reset();
}

void copy_buffer(
    CommandRecorder& transfer_command_recorder,
    const AllocatedBuffer& source_buffer,
    const AllocatedBuffer& dest_buffer)
{
    transfer_command_recorder.begin();
    VkCommandBuffer& command_buffer = transfer_command_recorder.get_command_buffer();

    VkBufferCopy copy_region = {
        .srcOffset = 0,
        .dstOffset = 0,
        .size = dest_buffer.size
    };

    vkCmdCopyBuffer(command_buffer, source_buffer.buffer, dest_buffer.buffer, 1, &copy_region);

    transfer_command_recorder.end();
    transfer_command_recorder.immediate_submit();
    transfer_command_recorder.reset();
}

void load_model(const std::filesystem::path& model_path, std::vector<Vertex>& vertex_buffer, std::vector<uint32_t>& index_buffer)
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, model_path.string().c_str()))
    {
        throw std::runtime_error(warn + err);
    }

    std::unordered_map<Vertex, uint32_t> unique_vertices{};

    for (const auto& shape : shapes)
    {
        for (const auto& index : shape.mesh.indices)
        {
            Vertex vertex{};

            vertex.pos = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2]
            };

            vertex.texCoord = {
                attrib.texcoords[2 * index.texcoord_index + 0],
                1.0f - attrib.texcoords[2 * index.texcoord_index + 1]
            };

            vertex.color = {1.0f, 1.0f, 1.0f};

            if (unique_vertices.count(vertex) == 0)
            {
                unique_vertices[vertex] = static_cast<uint32_t>(vertex_buffer.size());
                vertex_buffer.push_back(vertex);
            }

            index_buffer.push_back(unique_vertices[vertex]);
        }
    }
}

}

}