#include <string/render/debug_line_pass.hpp>

#include <cstring>

#include <string/gpu/pipeline_builder.hpp>

namespace string::render
{
using namespace String;

debug_line_pass::debug_line_pass(engine_context& ctx, std::shared_ptr<const MeshOverlayStats> stats,
                                 VkSampleCountFlagBits samples)
: device_(ctx.device), allocator_(ctx.allocator), stats_(std::move(stats)),
  frames_in_flight_(ctx.frames_in_flight)
{
    push_range_ = { VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4) };

    const VkVertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(::string::debug::LineVertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    const std::vector<VkVertexInputAttributeDescription> attrs = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(::string::debug::LineVertex, pos) },
        { 1, 0, VK_FORMAT_R32_UINT, offsetof(::string::debug::LineVertex, rgba) },
    };

    const auto build = [&](bool depth_test) {
        ::string::gpu::pipeline p{};
        p.push_constants = push_range_;
        p.pipeline_layout = ::string::gpu::pipeline_layout_builder()
            .set_descriptor_set_layout({})
            .set_push_constant_ranges({ push_range_ })
            .build(device_);
        p.pipeline = ::string::gpu::pipeline_builder(device_)
            .add_vertex_shader(ctx.resources_path / "shaders/debug_line.vert.spv")
            .add_fragment_shader(ctx.resources_path / "shaders/debug_line.frag.spv")
            .set_vertex_binding(binding, attrs)
            .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
            .set_tessellation()
            .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .set_multisampling(samples)
            // Depth-tested variant reads the scene depth (reverse-Z GREATER_OR_EQUAL) but never
            // writes it; overlay variant disables the test so it draws over everything.
            .enable_depth_stencil(depth_test, false)
            .enable_color_blending()
            .build_graphics_pipeline(p.pipeline_layout);
        p.pipeline_type = ::string::gpu::pipeline_type::GRAPHICS;
        return p;
    };
    depth_pipeline_ = build(true);
    overlay_pipeline_ = build(false);

    // Per-frame vertex ring: depth verts occupy [0, kMaxVerts), overlay [kMaxVerts, 2*kMaxVerts).
    vertex_buffers_.resize(frames_in_flight_);
    vertex_mapped_.resize(frames_in_flight_);
    depth_counts_.assign(frames_in_flight_, 0);
    overlay_counts_.assign(frames_in_flight_, 0);
    const VkDeviceSize buf_bytes = VkDeviceSize(kMaxVerts) * 2 * sizeof(::string::debug::LineVertex);
    for (uint32_t f = 0; f < frames_in_flight_; ++f)
    {
        vertex_buffers_[f] = allocator_.create_resource(::string::gpu::buffer_info{
            .size = buf_bytes,
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
            .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                              | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        });
        vertex_mapped_[f] = allocator_.get_buffer(vertex_buffers_[f]).allocation_info.pMappedData;
    }
}

debug_line_pass::~debug_line_pass()
{
    for (::string::gpu::resource_id id : vertex_buffers_) allocator_.destroy_resource(id);
    vkDestroyPipeline(device_.get_device(), depth_pipeline_.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), depth_pipeline_.pipeline_layout, nullptr);
    vkDestroyPipeline(device_.get_device(), overlay_pipeline_.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), overlay_pipeline_.pipeline_layout, nullptr);
}

// Same MSAA color+depth group as geometry: draw into the scene colour, read the scene depth.
void debug_line_pass::declare(::string::frame_graph& fg, ::string::gpu::image color,
                              ::string::gpu::image depth)
{
    fg.pass("debug_line")
      .color(color)
      .depth_read(depth)
      .raster([this](::string::pass_context& ctx) { record(ctx); });
}

void debug_line_pass::upload(std::uint32_t frame,
                             std::span<const ::string::debug::LineVertex> depth,
                             std::span<const ::string::debug::LineVertex> overlay)
{
    const uint32_t dn = std::min<uint32_t>(kMaxVerts, uint32_t(depth.size()));
    const uint32_t on = std::min<uint32_t>(kMaxVerts, uint32_t(overlay.size()));
    depth_counts_[frame] = dn;
    overlay_counts_[frame] = on;
    auto* base = static_cast<::string::debug::LineVertex*>(vertex_mapped_[frame]);
    if (dn) std::memcpy(base, depth.data(), dn * sizeof(::string::debug::LineVertex));
    if (on) std::memcpy(base + kMaxVerts, overlay.data(), on * sizeof(::string::debug::LineVertex));
}

void debug_line_pass::record(::string::pass_context& pctx)
{
    ::string::gpu::command_recorder& recorder = pctx.rec;
    if (pctx.frame_slot >= frames_in_flight_) return;

    // Snapshot the frame's debug-draw ring under its lock, then upload into this frame's buffer.
    ::string::debug::DebugDrawContext& ctx = ::string::debug::context();
    std::vector<::string::debug::LineVertex> depth, overlay;
    {
        std::lock_guard lock(ctx.mutex());
        auto d = ctx.depth_vertices();
        auto o = ctx.overlay_vertices();
        depth.assign(d.begin(), d.end());
        overlay.assign(o.begin(), o.end());
    }
    upload(pctx.frame_slot, depth, overlay);

    const glm::mat4 view_proj = stats_ ? stats_->view_proj : glm::mat4(1.0f);
    const VkDeviceSize depth_off = 0;
    const VkDeviceSize overlay_off = VkDeviceSize(kMaxVerts) * sizeof(::string::debug::LineVertex);
    const VkBuffer vb = allocator_.get_buffer(vertex_buffers_[pctx.frame_slot]).buffer;

    const auto draw = [&](const ::string::gpu::pipeline& p, VkDeviceSize offset, uint32_t count) {
        if (count == 0) return;
        recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
        recorder.push_constants(p.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                sizeof(glm::mat4), &view_proj);
        recorder.bind_vertex_buffers(0, 1, &vb, &offset);
        recorder.draw(count, 1, 0, 0);
    };
    draw(depth_pipeline_, depth_off, depth_counts_[pctx.frame_slot]);
    draw(overlay_pipeline_, overlay_off, overlay_counts_[pctx.frame_slot]);
}

}  // namespace string::render
