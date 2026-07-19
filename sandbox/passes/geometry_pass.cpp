#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <string/core/job_system.hpp>
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

namespace
{

// A texture decoded to RGBA8 on a worker thread, ready for a (single-threaded) GPU upload. The
// pixels are owned by stb_image and freed via its own deallocator when this is destroyed, so
// decode jobs can run in parallel without an extra copy.
struct DecodedTexture
{
    struct StbiDeleter { void operator()(stbi_uc* p) const { stbi_image_free(p); } };
    std::unique_ptr<stbi_uc, StbiDeleter> pixels;
    int width = 0;
    int height = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
};

// Decode one texture source (file or embedded bytes) to RGBA8. Pure CPU work — safe to run on a
// job thread. Throws on failure (captured by the job's future, rethrown at .get()).
DecodedTexture decode_texture(const GltfTexture& source)
{
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

    DecodedTexture decoded;
    decoded.pixels.reset(pixels);
    decoded.width = width;
    decoded.height = height;
    decoded.format = source.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    return decoded;
}

// Records the (already decoded) texture's upload into a device-local image and binds it into the
// bindless table. Stays on the main thread — allocation + command recording are single-threaded.
void upload_decoded(string::gpu::resource_allocator& allocator,
                    string::gpu::descriptor_table& descriptor_table, TransferBatch& transfer,
                    const DecodedTexture& decoded,
                    string::gpu::resource_id& out_image, uint32_t& out_slot)
{
    out_image = allocator.create_resource(string::gpu::image_info{
        .extent = { static_cast<uint32_t>(decoded.width), static_cast<uint32_t>(decoded.height), 1 },
        .format = decoded.format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    // upload_image copies the pixels into staging immediately; the batch streams it to the GPU
    // asynchronously (bounded by its staging budget), so no per-texture stall.
    const VkDeviceSize size = static_cast<VkDeviceSize>(decoded.width) * decoded.height * 4;
    transfer.upload_image(decoded.pixels.get(), size, out_image);

    descriptor_table.bind(out_image, string::gpu::descriptor_type::TEXTURE);
    out_slot = descriptor_table.get_binding_slot(out_image, string::gpu::descriptor_type::TEXTURE);
}

}  // namespace

GeometryPass::GeometryPass(PassContext& context, const std::filesystem::path& model_path)
: device_(context.device)
, allocator_(context.allocator)
, descriptor_table_(context.descriptor_table)
, input_map_(context.input_map)
, frames_in_flight_(context.frames_in_flight)
{
    // Parse the glTF (fast: buffers + resolved texture sources + materials), then overlap the
    // two expensive stages: texture decode runs on the worker pool while this thread flattens
    // the geometry. Texture decode dominates load, so hiding the flatten (and the vertex/index
    // upload) under it is effectively free.
    const auto load_start = std::chrono::steady_clock::now();
    GltfParsed parsed = parse_gltf(context.resources_path / model_path);

    // Kick off every texture decode now, before flattening — the jobs run on the pool while the
    // main thread does the geometry work below. Each decode is independent CPU work; the GPU
    // upload stays on this thread (VMA + command recording aren't thread-safe), collected in
    // order and streamed asynchronously by the transfer batch.
    string::core::job_system decode_pool;
    std::vector<std::future<DecodedTexture>> decode_jobs;
    decode_jobs.reserve(parsed.textures.size());
    for (std::size_t i = 0; i < parsed.textures.size(); ++i)
    {
        const GltfTexture* source = &parsed.textures[i];
        decode_jobs.push_back(decode_pool.enqueue([source]() { return decode_texture(*source); }));
    }

    // Flatten geometry on this thread (overlaps the decode jobs above).
    const GltfGeometry geometry = flatten_geometry(parsed);
    materials_ = parsed.materials;
    draws_ = geometry.draws;

    STRING_LOG_INFO("glTF loaded: {} vertices, {} indices, {} draws, {} materials, {} textures",
                    geometry.vertices.size(), geometry.indices.size(), geometry.draws.size(),
                    parsed.materials.size(), parsed.textures.size());

    // Upload the shared vertex + index buffers (device-local, recorded into the shared batch).
    const uint32_t vertex_size = sizeof(geometry.vertices[0]) * geometry.vertices.size();
    const uint32_t index_size = sizeof(geometry.indices[0]) * geometry.indices.size();

    vertex_buffer_ = allocator_.create_resource(string::gpu::buffer_info{
        .size = vertex_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    context.transfer.upload_buffer(geometry.vertices.data(), vertex_size, vertex_buffer_);

    index_buffer_ = allocator_.create_resource(string::gpu::buffer_info{
        .size = index_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    context.transfer.upload_buffer(geometry.indices.data(), index_size, index_buffer_);

    // Collect the (already running) decodes in order and record their uploads.
    texture_images_.resize(parsed.textures.size());
    texture_slots_.resize(parsed.textures.size());
    for (std::size_t i = 0; i < parsed.textures.size(); ++i)
    {
        const DecodedTexture decoded = decode_jobs[i].get();
        upload_decoded(allocator_, descriptor_table_, context.transfer, decoded,
                       texture_images_[i], texture_slots_[i]);
    }
    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - load_start).count();
    STRING_LOG_INFO("[load] total ({} textures, {} decode workers): {} ms",
                    parsed.textures.size(), decode_pool.worker_count(), load_ms);

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
    // The renderer drains the transfer batch (wait_idle) once after all passes are built; no
    // per-pass flush needed here.

    // --- GPU-driven draw data ------------------------------------------------------------------
    // Two per-draw buffers, built once: DrawData (transform + material + texture slot) that the
    // vertex shader indexes by gl_InstanceIndex, and CullDraw (world AABB + command fields) that
    // the cull compute shader reads to emit the frame's indirect commands (see record_compute).
    draw_count_ = static_cast<uint32_t>(draws_.size());
    if (draw_count_ > 0)
    {
        std::vector<DrawData> draw_data(draws_.size());
        std::vector<CullDraw> cull_data(draws_.size());
        for (std::size_t i = 0; i < draws_.size(); ++i)
        {
            const GltfDraw& draw = draws_[i];
            DrawData& data = draw_data[i];
            data.model = draw.transform;
            if (draw.material >= 0)
            {
                const GltfMaterial& material = materials_[draw.material];
                data.base_color = material.base_color_factor;
                data.texture_slot = material.base_color_texture >= 0
                    ? texture_slots_[material.base_color_texture]
                    : white_slot_;
            }
            else
            {
                data.base_color = glm::vec4(1.0f);
                data.texture_slot = white_slot_;
            }

            cull_data[i] = CullDraw{
                .aabb_min = glm::vec4(draw.aabb_min, 1.0f),
                .aabb_max = glm::vec4(draw.aabb_max, 1.0f),
                .index_count = draw.index_count,
                .first_index = draw.index_offset,
                .vertex_offset = 0,
                .draw_id = static_cast<uint32_t>(i),   // -> firstInstance -> gl_InstanceIndex
            };
        }

        const VkDeviceSize draw_data_size = sizeof(DrawData) * draw_data.size();
        draw_data_buffer_ = allocator_.create_resource(string::gpu::buffer_info{
            .size = draw_data_size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
        });
        context.transfer.upload_buffer(draw_data.data(), draw_data_size, draw_data_buffer_);
        descriptor_table_.bind(draw_data_buffer_, string::gpu::descriptor_type::STORAGE_BUFFER);
        draw_data_slot_ = descriptor_table_.get_binding_slot(
            draw_data_buffer_, string::gpu::descriptor_type::STORAGE_BUFFER);

        // Cull input (device-addressed so the compute shader reaches it by pointer).
        const VkDeviceSize cull_size = sizeof(CullDraw) * cull_data.size();
        cull_draw_buffer_ = allocator_.create_resource(string::gpu::buffer_info{
            .size = cull_size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                   | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
        });
        context.transfer.upload_buffer(cull_data.data(), cull_size, cull_draw_buffer_);

        // Cull output: the compute shader fills one command per draw each frame, drawn via
        // vkCmdDrawIndexedIndirect. One per frame-in-flight so a new frame's compute doesn't
        // clobber a still-in-flight frame's draw.
        culled_indirect_buffers_.resize(frames_in_flight_);
        for (uint32_t f = 0; f < frames_in_flight_; ++f)
        {
            culled_indirect_buffers_[f] = allocator_.create_resource(string::gpu::buffer_info{
                .size = sizeof(VkDrawIndexedIndirectCommand) * draw_count_,
                .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                       | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
                .allocation_flags = {},
            });
        }

        // Cull compute pipeline: push-constant only (buffers reached by device address).
        const VkPushConstantRange cull_push_range = {
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(CullPush),
        };
        cull_pipeline_.pipeline_layout = string::gpu::pipeline_layout_builder()
            .set_push_constant_ranges({ cull_push_range })
            .build(device_);
        cull_pipeline_.pipeline = string::gpu::pipeline_builder(device_, string::gpu::pipeline_type::COMPUTE)
            .add_compute_shader(context.resources_path / "shaders/cull.comp.spv")
            .build_compute_pipeline(cull_pipeline_.pipeline_layout);
        cull_pipeline_.pipeline_type = string::gpu::pipeline_type::COMPUTE;
    }

    // Frame the whole model with the engine camera: compute the AABB, then let it position
    // itself to fit. Bind the conventional fly controls (WASD + Space/Ctrl + Shift) onto the
    // shared InputMap so update() can read them by action name.
    glm::vec3 aabb_min(std::numeric_limits<float>::max());
    glm::vec3 aabb_max(std::numeric_limits<float>::lowest());
    for (const auto& vertex : geometry.vertices)
    {
        aabb_min = glm::min(aabb_min, vertex.pos);
        aabb_max = glm::max(aabb_max, vertex.pos);
    }
    if (geometry.vertices.empty())
    {
        aabb_min = glm::vec3(-1.0f);
        aabb_max = glm::vec3(1.0f);
    }
    String::Camera::bind_default_controls(input_map_);
    // Debug: F freezes/unfreezes the culling frustum (see update()).
    input_map_.bind_button("freeze_culling", String::KeyCode::F);
    camera_.frame_bounds(aabb_min, aabb_max);

    // Declared graph usages: the shared buffers + the render targets. The uploaded textures are
    // static SHADER_READ_ONLY inputs (transitioned once by the transfer batch), so they're not
    // graph-tracked and don't need declaring here.
    usages = {
        { vertex_buffer_,       Access::VertexRead,  VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT },
        { index_buffer_,        Access::IndexRead,   VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT },
        { draw_data_buffer_,    Access::StorageRead, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT },
        { context.color_target, Access::ColorWrite,  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT },
        { context.depth_target, Access::DepthWrite,  VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT },
    };
    // The indirect buffer's DRAW_INDIRECT read is covered by the transfer batch's global barrier
    // (like the static textures); once buffer state is graph-tracked it can be declared here too.

    // view_proj + drawdata_slot are read only by the vertex stage now (the fragment stage reads
    // the material tint/slot from varyings).
    const VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
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

    if (draw_count_ > 0)
    {
        vkDestroyPipeline(device_.get_device(), cull_pipeline_.pipeline, nullptr);
        vkDestroyPipelineLayout(device_.get_device(), cull_pipeline_.pipeline_layout, nullptr);

        descriptor_table_.unbind(draw_data_buffer_, string::gpu::descriptor_type::STORAGE_BUFFER);
        allocator_.destroy_resource(draw_data_buffer_);
        allocator_.destroy_resource(cull_draw_buffer_);
        for (const string::gpu::resource_id buffer : culled_indirect_buffers_)
            allocator_.destroy_resource(buffer);
    }

    allocator_.destroy_resource(index_buffer_);
    allocator_.destroy_resource(vertex_buffer_);
}

void GeometryPass::update(float delta_time, uint16_t current_frame)
{
    (void)current_frame;

    const float aspect = screen_size.height == 0
        ? 1.0f
        : screen_size.width / static_cast<float>(screen_size.height);
    camera_.update(input_map_, delta_time, aspect);

    // Toggle the debug frozen culling frustum. On freeze, snapshot the current view-projection;
    // the camera keeps moving but the cull test stays against the snapshot, so culled geometry
    // becomes visible as it leaves the frozen view.
    if (input_map_.pressed("freeze_culling"))
    {
        cull_frozen_ = !cull_frozen_;
        if (cull_frozen_)
        {
            frozen_cull_view_proj_ = camera_.view_proj();
        }
        STRING_LOG_INFO("Cull frustum {}", cull_frozen_ ? "FROZEN (debug)" : "live");
    }
}

bool GeometryPass::record_compute(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    if (draw_count_ == 0)
    {
        return false;
    }

    VkCommandBuffer& command_buffer = recorder.get_command_buffer();
    const string::gpu::resource_id indirect_id = culled_indirect_buffers_[current_frame];

    // Dispatch one thread per draw: each writes its indirect command (instanceCount 0 if the
    // draw's AABB is outside the camera frustum). The compute -> draw-indirect barrier is emitted
    // by the renderer after the compute prepass.
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipeline_.pipeline);
    const CullPush push{
        .view_proj = cull_frozen_ ? frozen_cull_view_proj_ : camera_.view_proj(),
        .cull_in = allocator_.get_buffer(cull_draw_buffer_).device_address,
        .indirect_out = allocator_.get_buffer(indirect_id).device_address,
        .draw_count = draw_count_,
    };
    vkCmdPushConstants(command_buffer, cull_pipeline_.pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPush), &push);
    vkCmdDispatch(command_buffer, (draw_count_ + 63) / 64, 1, 1);
    return true;
}

void GeometryPass::record(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    VkCommandBuffer& command_buffer = recorder.get_command_buffer();

    if (draw_count_ == 0)
    {
        return;
    }

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

    // Per-frame camera + the per-draw data buffer's bindless slot; the rest is on the GPU.
    const GeometryPush push{ .view_proj = camera_.view_proj(), .drawdata_slot = draw_data_slot_ };
    vkCmdPushConstants(
        command_buffer, pipeline_.pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GeometryPush), &push);

    // Draw every command the cull compute shader emitted this frame; culled draws carry
    // instanceCount 0 so the GPU skips them. Each selects its DrawData via firstInstance ->
    // gl_InstanceIndex.
    const auto& culled_indirect = allocator_.get_buffer(culled_indirect_buffers_[current_frame]);
    vkCmdDrawIndexedIndirect(command_buffer, culled_indirect.buffer, 0, draw_count_,
                             sizeof(VkDrawIndexedIndirectCommand));
}

}  // namespace sandbox
