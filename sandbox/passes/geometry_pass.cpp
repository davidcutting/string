#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include <string/core/logger.hpp>
#include <string/gpu/command_recorder.hpp>
#include "geometry_pass.hpp"
#include <string/gpu/pipeline_builder.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include "string/gpu/descriptor_allocator.hpp"
#include "string/gpu/resource.hpp"
#include "vulkan/vulkan_core.h"

// The single stb_image implementation for the sandbox lives here (this pass decodes textures).
#define STB_IMAGE_IMPLEMENTATION
#include <string/core/stb_image.h>

namespace sandbox
{
using namespace String;

void GeometryPass::upload_texture(
        PassContext& context, const GltfTexture& source,
        string::gpu::resource_id& out_image, uint32_t& out_slot)
{
    // Decode now (from disk or the embedded bytes) so only one texture is resident at a time.
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = source.file.empty()
        ? stbi_load_from_memory(source.encoded.data(), static_cast<int>(source.encoded.size()),
                                &width, &height, &channels, STBI_rgb_alpha)
        : stbi_load(source.file.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels)
    {
        throw std::runtime_error("gltf: failed to decode texture " +
            (source.file.empty() ? std::string("<embedded>") : source.file.string()));
    }

    const VkFormat format = source.srgb
        ? VK_FORMAT_R8G8B8A8_SRGB
        : VK_FORMAT_R8G8B8A8_UNORM;

    out_image = allocator_.create_resource(string::gpu::image_info{
        .extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 },
        .format = format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    // upload_image copies the pixels into staging immediately, so they can be freed right after.
    const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;
    context.transfer.upload_image(pixels, size, out_image);
    stbi_image_free(pixels);

    descriptor_table_.bind(out_image, string::gpu::descriptor_type::TEXTURE);
    out_slot = descriptor_table_.get_binding_slot(out_image, string::gpu::descriptor_type::TEXTURE);

    // Submit + free this texture's staging now, so the batch never holds all of Sponza's
    // textures at once (that peak — decoded + staging + GPU images — exhausts memory).
    context.transfer.flush();
}

GeometryPass::GeometryPass(PassContext& context, const std::filesystem::path& model_path)
: device_(context.device)
, allocator_(context.allocator)
, descriptor_table_(context.descriptor_table)
, input_(context.input)
{
    // Parse the glTF into a flat, GPU-ready model (shared vertex/index buffers + draw list).
    const GltfModel model = load_gltf(context.resources_path / model_path);
    materials_ = model.materials;
    draws_ = model.draws;

    STRING_LOG_INFO("glTF loaded: {} vertices, {} indices, {} draws, {} materials, {} textures",
                    model.vertices.size(), model.indices.size(), model.draws.size(),
                    model.materials.size(), model.textures.size());

    // Upload the shared vertex + index buffers (device-local, recorded into the shared batch).
    const uint32_t vertex_size = sizeof(model.vertices[0]) * model.vertices.size();
    const uint32_t index_size = sizeof(model.indices[0]) * model.indices.size();

    vertex_buffer_ = allocator_.create_resource(string::gpu::buffer_info{
        .size = vertex_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    context.transfer.upload_buffer(model.vertices.data(), vertex_size, vertex_buffer_);

    index_buffer_ = allocator_.create_resource(string::gpu::buffer_info{
        .size = index_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    context.transfer.upload_buffer(model.indices.data(), index_size, index_buffer_);

    // Upload every material texture into its own bindless slot, one at a time (each decode ->
    // upload -> flush -> free bounds peak memory; see upload_texture).
    texture_images_.resize(model.textures.size());
    texture_slots_.resize(model.textures.size());
    for (std::size_t i = 0; i < model.textures.size(); ++i)
    {
        upload_texture(context, model.textures[i], texture_images_[i], texture_slots_[i]);
    }
    STRING_LOG_INFO("Uploaded {} textures", model.textures.size());

    // 1x1 white fallback for draws without a base-color texture (factor still tints it).
    const std::array<uint8_t, 4> white_pixel = { 255, 255, 255, 255 };
    white_image_ = allocator_.create_resource(string::gpu::image_info{
        .extent = { 1, 1, 1 },
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    context.transfer.upload_image(white_pixel.data(), white_pixel.size(), white_image_);
    descriptor_table_.bind(white_image_, string::gpu::descriptor_type::TEXTURE);
    white_slot_ = descriptor_table_.get_binding_slot(white_image_, string::gpu::descriptor_type::TEXTURE);
    context.transfer.flush();

    // Frame the whole model: AABB -> look at its centre from far enough out to fit it.
    glm::vec3 aabb_min(std::numeric_limits<float>::max());
    glm::vec3 aabb_max(std::numeric_limits<float>::lowest());
    for (const auto& vertex : model.vertices)
    {
        aabb_min = glm::min(aabb_min, vertex.pos);
        aabb_max = glm::max(aabb_max, vertex.pos);
    }
    if (model.vertices.empty())
    {
        aabb_min = glm::vec3(-1.0f);
        aabb_max = glm::vec3(1.0f);
    }
    const glm::vec3 center = (aabb_min + aabb_max) * 0.5f;
    const float radius = glm::max(glm::length(aabb_max - aabb_min) * 0.5f, 0.001f);
    // Start outside the AABB looking at its centre; derive yaw/pitch from that direction.
    camera_pos_ = center + glm::normalize(glm::vec3(1.0f, 0.35f, 1.0f)) * radius * 1.1f;
    const glm::vec3 to_center = glm::normalize(center - camera_pos_);
    camera_yaw_ = std::atan2(to_center.z, to_center.x);
    camera_pitch_ = std::asin(glm::clamp(to_center.y, -1.0f, 1.0f));
    move_speed_ = radius * 0.5f;   // ~2 s to cross the scene; Shift sprints
    camera_near_ = radius * 0.01f;
    camera_far_ = radius * 4.0f;

    // Declared graph usages: the shared buffers + the render targets. The uploaded textures are
    // static SHADER_READ_ONLY inputs (transitioned once by the transfer batch), so they're not
    // graph-tracked and don't need declaring here.
    usages = {
        { vertex_buffer_,       Access::VertexRead,  VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT },
        { index_buffer_,        Access::IndexRead,   VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT },
        { context.color_target, Access::ColorWrite,  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT },
        { context.depth_target, Access::DepthWrite,  VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT },
    };

    const VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(GeometryPush),
    };

    pipeline_.pipeline_layout = string::gpu::pipeline_layout_builder()
        .set_descriptor_set_layout({ descriptor_table_.get_layout() })
        .set_push_constant_ranges({ push_constant_range })
        .build(device_);

    const auto binding_description = Vertex::getBindingDescription();
    const auto attribute_descriptions = Vertex::getAttributeDescriptions();
    pipeline_.pipeline = string::gpu::pipeline_builder(device_)
        .add_vertex_shader(context.resources_path / "shaders/3d_shader.vert.spv")
        .add_fragment_shader(context.resources_path / "shaders/3d_shader.frag.spv")
        .set_vertex_binding(binding_description, attribute_descriptions)
        .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .set_tessellation()
        // Back-face cull with a CCW front face: glTF winds front faces CCW, and this pipeline's
        // GLM projection + Y-flip keep that winding front-facing in the framebuffer (same setup
        // the OBJ viking-room rendered correctly with). CLOCKWISE here culls the wrong faces.
        .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .set_multisampling()
        .enable_depth_stencil()
        .enable_color_blending()
        .build_graphics_pipeline(pipeline_.pipeline_layout);
    pipeline_.pipeline_type = string::gpu::pipeline_type::GRAPHICS;
}

GeometryPass::~GeometryPass()
{
    vkDestroyPipeline(device_.get_device(), pipeline_.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), pipeline_.pipeline_layout, nullptr);

    descriptor_table_.unbind(white_image_, string::gpu::descriptor_type::TEXTURE);
    allocator_.destroy_resource(white_image_);
    for (const string::gpu::resource_id image : texture_images_)
    {
        descriptor_table_.unbind(image, string::gpu::descriptor_type::TEXTURE);
        allocator_.destroy_resource(image);
    }

    allocator_.destroy_resource(index_buffer_);
    allocator_.destroy_resource(vertex_buffer_);
}

void GeometryPass::update(float delta_time, uint16_t current_frame)
{
    (void)current_frame;

    // Mouse-look: yaw from horizontal motion, pitch from vertical (inverted), pitch clamped so
    // we can't flip over. mouse_delta() is zero while the cursor is released (see Input).
    const glm::vec2 mouse = input_.mouse_delta();
    constexpr float sensitivity = 0.0025f;
    camera_yaw_ += mouse.x * sensitivity;
    camera_pitch_ -= mouse.y * sensitivity;
    const float pitch_limit = glm::radians(89.0f);
    camera_pitch_ = glm::clamp(camera_pitch_, -pitch_limit, pitch_limit);

    const glm::vec3 forward = {
        std::cos(camera_pitch_) * std::cos(camera_yaw_),
        std::sin(camera_pitch_),
        std::cos(camera_pitch_) * std::sin(camera_yaw_),
    };
    const glm::vec3 world_up = { 0.0f, 1.0f, 0.0f };
    const glm::vec3 right = glm::normalize(glm::cross(forward, world_up));

    // WASD moves in the look plane; Space/Ctrl move straight up/down; Shift sprints.
    glm::vec3 move{ 0.0f };
    if (input_.key_down(KeyCode::W)) move += forward;
    if (input_.key_down(KeyCode::S)) move -= forward;
    if (input_.key_down(KeyCode::D)) move += right;
    if (input_.key_down(KeyCode::A)) move -= right;
    if (input_.key_down(KeyCode::SPACE)) move += world_up;
    if (input_.key_down(KeyCode::LEFT_CONTROL)) move -= world_up;

    float speed = move_speed_;
    if (input_.key_down(KeyCode::LEFT_SHIFT)) speed *= 4.0f;
    if (glm::length(move) > 0.0f)
        camera_pos_ += glm::normalize(move) * speed * delta_time;

    const float aspect = screen_size.height == 0
        ? 1.0f
        : screen_size.width / static_cast<float>(screen_size.height);

    const glm::mat4 view = glm::lookAt(camera_pos_, camera_pos_ + forward, world_up);
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), aspect, camera_near_, camera_far_);
    proj[1][1] *= -1;   // GLM is OpenGL-handed; flip Y for Vulkan.

    view_proj_ = proj * view;
}

void GeometryPass::record(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    (void)current_frame;
    VkCommandBuffer& command_buffer = recorder.get_command_buffer();

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.pipeline);

    const auto& vertex_buffer = allocator_.get_buffer(vertex_buffer_);
    const auto& index_buffer = allocator_.get_buffer(index_buffer_);

    VkBuffer vertex_buffers[] = { vertex_buffer.buffer };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(command_buffer, 0, 1, vertex_buffers, offsets);
    vkCmdBindIndexBuffer(command_buffer, index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    VkDescriptorSet set = descriptor_table_.get_set();
    vkCmdBindDescriptorSets(
        command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_.pipeline_layout, 0, 1, &set, 0, nullptr);

    // One indexed draw per primitive instance; material + transform ride in the push constant.
    for (const GltfDraw& draw : draws_)
    {
        GeometryPush push{};
        push.mvp = view_proj_ * draw.transform;

        if (draw.material >= 0)
        {
            const GltfMaterial& material = materials_[draw.material];
            push.base_color = material.base_color_factor;
            push.texture_slot = material.base_color_texture >= 0
                ? texture_slots_[material.base_color_texture]
                : white_slot_;
        }
        else
        {
            push.base_color = glm::vec4(1.0f);
            push.texture_slot = white_slot_;
        }

        vkCmdPushConstants(
            command_buffer, pipeline_.pipeline_layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(GeometryPush), &push);

        vkCmdDrawIndexed(command_buffer, draw.index_count, 1, draw.index_offset, 0, 0);
    }
}

}  // namespace sandbox
