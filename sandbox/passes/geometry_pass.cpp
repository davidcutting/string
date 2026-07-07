#include <cstdint>
#include <string/vulkan/command_recorder.hpp>
#include "geometry_pass.hpp"
#include <string/vulkan/pipeline_builder.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include <algorithm>
#include <cstring>
#include "string/vulkan/descriptor_allocator.hpp"
#include "string/vulkan/resource.hpp"
#include "vulkan/vulkan_core.h"

#define STB_IMAGE_IMPLEMENTATION
#include <string/core/stb_image.h>

namespace sandbox
{
using namespace String;

GeometryPass::GeometryPass(
        PassContext& context,
        const std::filesystem::path& model_path,
        const std::filesystem::path& texture_path)
: device_(context.device)
, allocator_(context.allocator)
, descriptor_table_(context.descriptor_table)
{
    DescriptorTable& descriptor_table = context.descriptor_table;
    TransferBatch& transfer = context.transfer;
    const std::filesystem::path& resources_path = context.resources_path;

    // Load model + texture into RAM.
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    vku::load_model(resources_path / model_path, vertices, indices);

    int tex_width;
    int tex_height;
    int tex_channels;
    const auto tex_path = resources_path / texture_path;
    stbi_uc* pixels = stbi_load(tex_path.string().c_str(), &tex_width, &tex_height, &tex_channels, STBI_rgb_alpha);
    if (!pixels)
    {
        throw std::runtime_error("Failed to load texture image!");
    }

    const uint32_t vertex_size = sizeof(vertices[0]) * vertices.size();
    const uint32_t index_size = sizeof(indices[0]) * indices.size();
    const uint32_t image_size = tex_width * tex_height * 4;

    // Upload mesh vertices + indices (device-local). Recorded into the shared batch; the
    // renderer submits every pass's uploads together in one go.
    scene_3d_.vertex_buffer = allocator_.create_resource(BufferInfo{
        .size = vertex_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    transfer.upload_buffer(vertices.data(), vertex_size, scene_3d_.vertex_buffer);

    scene_3d_.index_buffer = allocator_.create_resource(BufferInfo{
        .size = index_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    transfer.upload_buffer(indices.data(), index_size, scene_3d_.index_buffer);
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
    // Recorded into the batch (transition -> copy -> transition). pixels is copied into
    // staging now, so it's safe to free immediately after.
    transfer.upload_image(pixels, image_size, texture_image_);
    stbi_image_free(pixels);

    // Bind the texture into the bindless table and remember its slot for the push constant.
    descriptor_table_.bind(texture_image_, DescriptorType::TEXTURE);
    texture_slot_ = descriptor_table_.get_binding_slot(texture_image_, DescriptorType::TEXTURE);

    // Declare the resources this pass reads for the (future) render graph. The color/depth
    // render-target writes are renderer-owned and get declared once attachments become
    // first-class graph resources (a later stage).
    usages = {
        { texture_image_,          Access::SampledRead, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT },
        { scene_3d_.vertex_buffer, Access::VertexRead,  VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT },
        { scene_3d_.index_buffer,  Access::IndexRead,   VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT },
        { context.color_target,    Access::ColorWrite,  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT },
        { context.depth_target,    Access::DepthWrite,  VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT },
    };

    const VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(GeometryPush),
    };

    pipeline_.pipeline_layout = PipelineLayoutBuilder()
        .set_descriptor_set_layout({ descriptor_table.get_layout() })
        .set_push_constant_ranges({ push_constant_range })
        .build(device_);

    // Textured mesh: vertex input from Vertex, depth test + write on, back-face culling.
    const auto binding_description = Vertex::getBindingDescription();
    const auto attribute_descriptions = Vertex::getAttributeDescriptions();
    pipeline_.pipeline = PipelineBuilder(device_)
        .add_vertex_shader(resources_path / "shaders/3d_shader.vert.spv")
        .add_fragment_shader(resources_path / "shaders/3d_shader.frag.spv")
        .set_vertex_binding(binding_description, attribute_descriptions)
        .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .set_tessellation()
        .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT)
        .set_multisampling()
        .enable_depth_stencil()
        .enable_color_blending()
        .build_graphics_pipeline(pipeline_.pipeline_layout);
    pipeline_.pipeline_type = PipelineType::GRAPHICS;
}

GeometryPass::~GeometryPass()
{
    vkDestroyPipeline(device_.get_device(), pipeline_.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), pipeline_.pipeline_layout, nullptr);

    descriptor_table_.unbind(texture_image_, DescriptorType::TEXTURE);

    allocator_.destroy_resource(scene_3d_.index_buffer);
    allocator_.destroy_resource(scene_3d_.vertex_buffer);
    allocator_.destroy_resource(texture_image_);
}

void GeometryPass::update(float delta_time, uint16_t current_frame)
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

void GeometryPass::record(CommandRecorder& recorder, uint16_t current_frame)
{
    (void)current_frame;
    VkCommandBuffer& command_buffer = recorder.get_command_buffer();

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.pipeline);

    auto& vertex_buffer = allocator_.get_buffer(scene_3d_.vertex_buffer);
    auto& index_buffer  = allocator_.get_buffer(scene_3d_.index_buffer);

    VkBuffer vb[]       = { vertex_buffer.buffer };
    VkDeviceSize offs[] = { 0 };
    vkCmdBindVertexBuffers(command_buffer, 0, 1, vb, offs);
    vkCmdBindIndexBuffer(command_buffer, index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    VkDescriptorSet set = descriptor_table_.get_set();
    vkCmdBindDescriptorSets(
        command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_.pipeline_layout, 0, 1, &set, 0, nullptr);

    vkCmdPushConstants(
        command_buffer, pipeline_.pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(GeometryPush), &push_);

    vkCmdDrawIndexed(command_buffer, scene_3d_.index_count, 1, 0, 0, 0);
}

}
