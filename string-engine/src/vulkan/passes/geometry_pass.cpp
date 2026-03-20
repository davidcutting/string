#include <cstdint>
#include <string/vulkan/command_recorder.hpp>
#include <string/vulkan/passes/geometry_pass.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include <cstring>
#include "string/vulkan/descriptor_allocator.hpp"
#include "string/vulkan/resource.hpp"
#include "vulkan/vulkan_core.h"

#define STB_IMAGE_IMPLEMENTATION
#include <string/core/stb_image.h>

namespace String
{

GeometryPass::GeometryPass(
        Device& device,
        ResourceAllocator& allocator,
        DescriptorTable& descriptor_table,
        CommandRecorder& streaming_recorder,
        const std::filesystem::path& resources_path,
        const uint16_t& frames_in_flight)
: device_(device)
, allocator_(allocator)
, descriptor_table_(descriptor_table)
{
    // Load scene data
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    // Load model into RAM
    vku::load_model(resources_path / MODEL_PATH, vertices, indices);

    // Load texture into RAM
    int tex_width;
    int tex_height;
    int tex_channels;
    const auto tex_path = resources_path / TEXTURE_PATH;
    stbi_uc* pixels = stbi_load(tex_path.string().c_str(), &tex_width, &tex_height, &tex_channels, STBI_rgb_alpha);
    if (!pixels)
    {
        throw std::runtime_error("Failed to load texture image!");
    }

    // Just make the staging buffer big enough for both vertex buffer, index buffer, and texture
    uint32_t vertex_size = sizeof(vertices[0]) * vertices.size();
    uint32_t index_size = sizeof(indices[0]) * indices.size();
    uint32_t image_size = tex_width * tex_height * 4;

    uint32_t max_buffer_size = std::max(vertex_size, index_size);
    uint32_t max_staging_size = std::max(max_buffer_size, image_size);
    
    upload_staging_ = allocator.create_staging(max_staging_size);
    auto& staging = allocator.get_buffer(upload_staging_);

    // Upload mesh vertices
    scene_3d_.vertex_buffer = allocator.create_persistent_resource(BufferInfo{
        .size = vertex_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    auto& vertex_buffer = allocator.get_buffer(scene_3d_.vertex_buffer);
    allocator.copy_data_to_buffer(vertices.data(), upload_staging_);
    
    vku::copy_buffer(streaming_recorder, staging, vertex_buffer);

    // Upload mesh indices
    scene_3d_.index_buffer = allocator.create_persistent_resource(BufferInfo{
        .size = index_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    auto& index_buffer = allocator.get_buffer(scene_3d_.index_buffer);
    allocator.copy_data_to_buffer(indices.data(), upload_staging_);
    vku::copy_buffer(streaming_recorder, staging, index_buffer);
    scene_3d_.index_count = indices.size();

    // Upload texture
    texture_image_ = allocator.create_persistent_resource(ImageInfo{
        .extent = {(uint32_t)tex_width, (uint32_t)tex_height, 1},
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = 0,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    auto& texture_image = allocator.get_image(texture_image_);
    vku::transition_image_layout(
        streaming_recorder,
        texture_image.image,
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    allocator.copy_data_to_buffer(pixels, upload_staging_);
    stbi_image_free(pixels);

    vku::copy_buffer_to_image(streaming_recorder, staging, texture_image, { (uint32_t)tex_width, (uint32_t)tex_height });
    vku::transition_image_layout(
        streaming_recorder,
        texture_image.image,
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    descriptor_table_.bind(texture_image_, DescriptorType::TEXTURE);

    // Delete no longer necessary staging buffer
    allocator.destroy_resource(upload_staging_);

    // Create Pipeline
    pipeline_3d_ = std::make_unique<Pipeline3D>(resources_path, device_, descriptor_table.get_layout());
}

GeometryPass::~GeometryPass()
{
    pipeline_3d_.reset();

    descriptor_table_.unbind(texture_image_, DescriptorType::TEXTURE);

    allocator_.destroy_resource(scene_3d_.index_buffer);
    allocator_.destroy_resource(scene_3d_.vertex_buffer);
    allocator_.destroy_resource(texture_image_);
}

void GeometryPass::update(const float& delta_time, const uint16_t& current_frame)
{
    const auto& camera_ubo_resource = allocator_.create_transient_resource(BufferInfo{
        .size = sizeof(Camera3D) * 1,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
        .allocation_flags = {},
    });

    // Re-bind new camera buffer
    descriptor_table_.bind(camera_ubo_resource, DescriptorType::UNIFORM);

    Camera3D camera = {
        .model = glm::rotate(glm::mat4(1.0f), delta_time * glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
        .view = glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
        .proj = glm::perspective(glm::radians(45.0f), screen_size.width / (float)screen_size.height, 0.1f, 10.0f)
    };
    camera.proj[1][1] *= -1;
    scene_3d_.camera = camera;

    allocator_.copy_data_to_buffer(&camera, camera_ubo_resource);
}

void GeometryPass::record(CommandRecorder& recorder, const uint16_t& current_frame)
{
    VkCommandBuffer& command_buffer = recorder.get_command_buffer();

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_3d_->get_pipeline());

    auto& vertex_buffer = allocator_.get_buffer(scene_3d_.vertex_buffer);
    auto& index_buffer  = allocator_.get_buffer(scene_3d_.index_buffer);

    VkBuffer vb[]       = { vertex_buffer.buffer };
    VkDeviceSize offs[] = { 0 };
    vkCmdBindVertexBuffers(command_buffer, 0, 1, vb, offs);
    vkCmdBindIndexBuffer(command_buffer, index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    VkDescriptorSet set = descriptor_table_.get_set();
    vkCmdBindDescriptorSets(
        command_buffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_3d_->get_pipeline_layout(),
        0, 1, &set,
        0, nullptr);

    vkCmdDrawIndexed(command_buffer, scene_3d_.index_count, 1, 0, 0, 0);
}

}