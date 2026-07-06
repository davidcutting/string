#include <cstdint>
#include <string/vulkan/command_recorder.hpp>
#include <string/vulkan/passes/geometry_pass.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include <algorithm>
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
    (void)frames_in_flight;

    // Load model + texture into RAM.
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    vku::load_model(resources_path / MODEL_PATH, vertices, indices);

    int tex_width;
    int tex_height;
    int tex_channels;
    const auto tex_path = resources_path / TEXTURE_PATH;
    stbi_uc* pixels = stbi_load(tex_path.string().c_str(), &tex_width, &tex_height, &tex_channels, STBI_rgb_alpha);
    if (!pixels)
    {
        throw std::runtime_error("Failed to load texture image!");
    }

    const uint32_t vertex_size = sizeof(vertices[0]) * vertices.size();
    const uint32_t index_size = sizeof(indices[0]) * indices.size();
    const uint32_t image_size = tex_width * tex_height * 4;

    // Each upload uses an exactly-sized staging buffer: copy_data_to_buffer copies the
    // *staging buffer's* whole size, so a shared oversized staging buffer would read past
    // the source data (a real OOB / segfault once the texture dwarfs the mesh). Each
    // vku::copy_* self-submits and waits, so the staging buffer is safe to free right after.
    const auto stage_and_copy_buffer = [&](const void* data, uint32_t size, ResourceID dst) {
        const ResourceID staging = allocator_.create_staging(size);
        allocator_.copy_data_to_buffer(data, staging);
        vku::copy_buffer(streaming_recorder, allocator_.get_buffer(staging), allocator_.get_buffer(dst));
        allocator_.destroy_resource(staging);
    };

    // Upload mesh vertices + indices (device-local).
    scene_3d_.vertex_buffer = allocator_.create_resource(BufferInfo{
        .size = vertex_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    stage_and_copy_buffer(vertices.data(), vertex_size, scene_3d_.vertex_buffer);

    scene_3d_.index_buffer = allocator_.create_resource(BufferInfo{
        .size = index_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    stage_and_copy_buffer(indices.data(), index_size, scene_3d_.index_buffer);
    scene_3d_.index_count = indices.size();

    // Upload texture (device-local, SRGB).
    texture_image_ = allocator_.create_resource(ImageInfo{
        .extent = {(uint32_t)tex_width, (uint32_t)tex_height, 1},
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    {
        auto& texture_image = allocator_.get_image(texture_image_);
        vku::transition_image_layout(
            streaming_recorder, texture_image.image, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        const ResourceID staging = allocator_.create_staging(image_size);
        allocator_.copy_data_to_buffer(pixels, staging);
        vku::copy_buffer_to_image(streaming_recorder, allocator_.get_buffer(staging), texture_image,
            { (uint32_t)tex_width, (uint32_t)tex_height });
        allocator_.destroy_resource(staging);

        vku::transition_image_layout(
            streaming_recorder, texture_image.image, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    stbi_image_free(pixels);

    // Bind the texture into the bindless table and remember its slot for the push constant.
    descriptor_table_.bind(texture_image_, DescriptorType::TEXTURE);
    texture_slot_ = descriptor_table_.get_binding_slot(texture_image_, DescriptorType::TEXTURE);

    const VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(GeometryPush),
    };
    pipeline_3d_ = std::make_unique<Pipeline3D>(
        resources_path, device_, descriptor_table.get_layout(), push_constant_range);
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
    (void)current_frame;

    const float aspect = screen_size.height == 0
        ? 1.0f
        : screen_size.width / static_cast<float>(screen_size.height);

    glm::mat4 model = glm::rotate(glm::mat4(1.0f), delta_time * glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    glm::mat4 view = glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 10.0f);
    proj[1][1] *= -1;   // GLM is OpenGL-handed; flip Y for Vulkan.

    push_.mvp = proj * view * model;
    push_.texture_slot = texture_slot_;
}

void GeometryPass::record(CommandRecorder& recorder, const uint16_t& current_frame)
{
    (void)current_frame;
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
        command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_3d_->get_pipeline_layout(), 0, 1, &set, 0, nullptr);

    vkCmdPushConstants(
        command_buffer, pipeline_3d_->get_pipeline_layout(),
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(GeometryPush), &push_);

    vkCmdDrawIndexed(command_buffer, scene_3d_.index_count, 1, 0, 0, 0);
}

}
