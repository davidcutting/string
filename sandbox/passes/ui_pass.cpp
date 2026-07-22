#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ui_pass.hpp"
#include "overlay_common.hpp"

#include <string/core/logger.hpp>
#include <string/core/text_measurer.hpp>
#include <string/gpu/pipeline_builder.hpp>

namespace sandbox
{
using namespace String;

namespace
{

// --- Shapes (rounded-rect SDF); matches `struct Shape` in shaders/ui_shader.vert -------------
struct GpuShape
{
    float rect[4];    // pos.xy, size.xy (px)
    float fill[4];    // linear rgba
    float stroke[4];  // linear rgba
    float params[4];  // corner_radius, stroke_width, _, _
};
struct ShapePush
{
    float screen_size[2];
    std::uint32_t shape_slot;
};

// Resolve an element's shape enum to the SDF corner radius: sharp rect = 0, rounded = its radius,
// circle/pill = half the smaller side.
float effective_radius(const string::element& e, float w, float h)
{
    switch (e.shape)
    {
        case string::shape::RECTANGLE:         return 0.0f;
        case string::shape::ROUNDED_RECTANGLE: return static_cast<float>(e.radius);
        case string::shape::CIRCLE:            return std::min(w, h) * 0.5f;
    }
    return 0.0f;
}

// Pack the non-text nodes of a laid-out tree into GPU shapes (text nodes are drawn as glyphs).
std::vector<GpuShape> pack_shapes(std::span<const string::layout_node> nodes)
{
    std::vector<GpuShape> shapes;
    shapes.reserve(nodes.size());
    for (const string::layout_node& n : nodes)
    {
        if (n.element.text != 0)
        {
            continue;  // text node -> glyphs, not a rect
        }
        const float w = static_cast<float>(n.box.dimension.width);
        const float h = static_cast<float>(n.box.dimension.height);
        const std::array<float, 4> fill = to_linear(n.element.color);
        const std::array<float, 4> stroke = to_linear(n.element.stroke_color);
        shapes.push_back(GpuShape{
            { static_cast<float>(n.box.x), static_cast<float>(n.box.y), w, h },
            { fill[0], fill[1], fill[2], fill[3] },
            { stroke[0], stroke[1], stroke[2], stroke[3] },
            { effective_radius(n.element, w, h), static_cast<float>(n.element.stroke_width), 0.0f, 0.0f },
        });
    }
    return shapes;
}

// --- Glyphs (SDF text); matches `struct Glyph` in shaders/text_shader.vert --------------------
struct GpuGlyph
{
    float rect[4];   // x, y, w, h (px)
    float uv[4];     // u0, v0, u1, v1
    float color[4];  // linear rgba
};
struct TextPush
{
    float screen_size[2];
    std::uint32_t glyph_slot;
    std::uint32_t atlas_slot;
};

// Shape the text nodes' strings into positioned glyph quads (see the phase-1 text pass).
std::vector<GpuGlyph> shape_glyphs(const string::font_atlas& atlas,
                                   std::span<const string::layout_node> nodes,
                                   std::span<const string::text_run> texts)
{
    std::vector<GpuGlyph> glyphs;
    for (const string::layout_node& n : nodes)
    {
        if (n.element.text == 0 || n.element.text >= texts.size())
        {
            continue;
        }
        const std::array<float, 4> color = to_linear(n.element.color);
        const std::string_view str = texts[n.element.text].str;

        const float origin_x = static_cast<float>(n.box.x);
        float pen_x = origin_x;
        float baseline_y = static_cast<float>(n.box.y) + atlas.ascent;
        for (const char c : str)
        {
            if (c == '\n')
            {
                pen_x = origin_x;
                baseline_y += atlas.line_height();
                continue;
            }
            const string::glyph_metrics& g = atlas.glyph(c);
            if (g.w > 0 && g.h > 0)
            {
                glyphs.push_back(GpuGlyph{
                    { pen_x + g.xoff, baseline_y + g.yoff, static_cast<float>(g.w), static_cast<float>(g.h) },
                    { g.u0, g.v0, g.u1, g.v1 },
                    { color[0], color[1], color[2], color[3] },
                });
            }
            pen_x += g.xadvance;
        }
    }
    return glyphs;
}

}  // namespace

void UIPass::make_ring(std::vector<Ring>& ring, std::uint32_t frames_in_flight,
                       std::uint32_t capacity, std::size_t stride)
{
    ring.resize(frames_in_flight);
    for (Ring& r : ring)
    {
        r.buffer = allocator_.create_resource(string::gpu::buffer_info{
            .size = capacity * stride,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
            .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                              | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        });
        descriptor_table_.bind(r.buffer, string::gpu::descriptor_type::STORAGE_BUFFER);
        r.slot = descriptor_table_.get_binding_slot(r.buffer, string::gpu::descriptor_type::STORAGE_BUFFER);
        r.mapped = allocator_.get_buffer(r.buffer).allocation_info.pMappedData;
    }
}

UIPass::UIPass(PassContext& context, std::shared_ptr<const string::font_atlas> atlas, Author author)
: device_(context.device)
, allocator_(context.allocator)
, descriptor_table_(context.descriptor_table)
, atlas_(std::move(atlas))
, author_(std::move(author))
, input_(context.input)
, shader_registry_(context.shader_registry)
{
    const std::filesystem::path& resources_path = context.resources_path;

    // --- SDF atlas: an R8 sampled image, uploaded once and bound bindlessly ---
    atlas_image_ = allocator_.create_resource(string::gpu::image_info{
        .extent = { atlas_->atlas_w, atlas_->atlas_h, 1 },
        .format = VK_FORMAT_R8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
        .mip_levels = 1,
    });
    context.transfer.upload_image(atlas_->pixels.data(), atlas_->pixels.size(), atlas_image_);
    descriptor_table_.bind(atlas_image_, string::gpu::descriptor_type::TEXTURE);
    atlas_slot_ = descriptor_table_.get_binding_slot(atlas_image_, string::gpu::descriptor_type::TEXTURE);
    const VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    if (vkCreateSampler(device_.get_device(), &sampler_info, nullptr, &atlas_sampler_) != VK_SUCCESS)
    {
        throw std::runtime_error("UIPass: failed to create atlas sampler");
    }
    descriptor_table_.update_texture(atlas_slot_, allocator_.get_image(atlas_image_).view, atlas_sampler_);

    // --- Per-frame ring buffers: shapes + glyphs ---
    make_ring(shape_ring_, context.frames_in_flight, kMaxShapes, sizeof(GpuShape));
    make_ring(glyph_ring_, context.frames_in_flight, kMaxGlyphs, sizeof(GpuGlyph));
    descriptor_set_ = descriptor_table_.get_set();

    usages = {
        { context.color_target, Access::ColorWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT },
    };

    // Both overlay pipelines share the same fixed state (instanced quads, no cull, MSAA, no depth,
    // alpha blend); only the shaders + reflected push-constant range differ. Build them via the
    // hot-reload registry so a save recompiles + swaps them. Set 0 stays the bindless table layout;
    // reflection drives the push-constant range.
    VkDescriptorSetLayout global_layout = descriptor_table_.get_layout();
    VkSampleCountFlagBits samples = context.sample_count;
    auto overlay_builder = [global_layout, samples](string::gpu::device& dev,
                                                    const string::gpu::compiled_program& compiled) {
        string::gpu::pipeline p{};
        p.push_constants = compiled.layout.push_constant;
        p.pipeline_layout = string::gpu::pipeline_layout_builder()
            .set_descriptor_set_layout({ global_layout })
            .set_push_constant_ranges({ compiled.layout.push_constant })
            .build(dev);

        string::gpu::pipeline_builder builder(dev);
        for (const auto& stage : compiled.stages)
        {
            if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT)
                builder.add_vertex_shader_spirv(stage.spirv, stage.entry_point);
            else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                builder.add_fragment_shader_spirv(stage.spirv, stage.entry_point);
        }
        p.pipeline = builder
            .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .set_tessellation()
            .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .set_multisampling(samples)
            .enable_depth_stencil(false, false)
            .enable_color_blending()
            .build_graphics_pipeline(p.pipeline_layout);
        p.pipeline_type = string::gpu::pipeline_type::GRAPHICS;
        return p;
    };

    shape_program_ = context.shader_registry.create(resources_path / "shaders" / "ui_shader.slang",
                                                    overlay_builder);
    text_program_ = context.shader_registry.create(resources_path / "shaders" / "text_shader.slang",
                                                   overlay_builder);
}

UIPass::~UIPass()
{
    const string::gpu::pipeline& shape_p = shape_program_->current();
    vkDestroyPipeline(device_.get_device(), shape_p.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), shape_p.pipeline_layout, nullptr);
    const string::gpu::pipeline& text_p = text_program_->current();
    vkDestroyPipeline(device_.get_device(), text_p.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), text_p.pipeline_layout, nullptr);
    for (Ring& r : shape_ring_)
    {
        descriptor_table_.unbind(r.buffer, string::gpu::descriptor_type::STORAGE_BUFFER);
        allocator_.destroy_resource(r.buffer);
    }
    for (Ring& r : glyph_ring_)
    {
        descriptor_table_.unbind(r.buffer, string::gpu::descriptor_type::STORAGE_BUFFER);
        allocator_.destroy_resource(r.buffer);
    }
    descriptor_table_.unbind(atlas_image_, string::gpu::descriptor_type::TEXTURE);
    allocator_.destroy_resource(atlas_image_);
    vkDestroySampler(device_.get_device(), atlas_sampler_, nullptr);
}

void UIPass::author_error_overlay()
{
    const std::vector<string::gpu::compile_error> errors = shader_registry_.current_errors();
    if (errors.empty())
    {
        error_lines_.clear();
        return;  // last compile succeeded — overlay clears
    }

    // Flatten the diagnostics into individual lines (Slang messages already carry file:line:col).
    error_lines_.clear();
    for (const string::gpu::compile_error& e : errors)
    {
        error_lines_.push_back("shader error: " + e.file.filename().string());
        std::string msg = e.message;
        std::size_t start = 0;
        while (start < msg.size())
        {
            const std::size_t nl = msg.find('\n', start);
            const std::size_t end = (nl == std::string::npos) ? msg.size() : nl;
            if (end > start)
            {
                error_lines_.push_back(msg.substr(start, end - start));
            }
            start = end + 1;
        }
    }

    using namespace string;
    element panel{};
    panel.color = { 40, 8, 8, 240 };          // dark red, near-opaque
    panel.stroke_color = { 243, 139, 168, 255 };
    panel.stroke_width = 2;
    panel.radius = 8;
    panel.shape = shape::ROUNDED_RECTANGLE;
    panel.sizing = size_fit();

    builder_.begin(panel, format{ .padding = { 10, 10, 10, 10 }, .gap = 2,
                                  .direction = direction::VERTICAL });
    for (const std::string& line : error_lines_)
    {
        element text_el{};
        text_el.color = { 243, 139, 168, 255 };  // readable red-pink
        text_el.sizing = size_fit();
        builder_.add_text(text_el, line, 20);
    }
    builder_.end();
}

void UIPass::update(float /*delta_time*/, uint16_t current_frame)
{
    if (screen_size.width == 0 || screen_size.height == 0 || current_frame >= shape_ring_.size())
    {
        return;  // not sized yet
    }

    // Author + lay out the whole UI. The callback adds children to a screen-filling root; it reads
    // last frame's hovered/focused ids to style, and the live input to route text to a focused field.
    builder_.clear();
    builder_.begin(string::format{ .padding = { 12, 12, 12, 12 }, .gap = 8,
                                   .direction = string::direction::VERTICAL });
    author_(builder_, UiContext{ input_, hovered_id_, focused_id_ });
    author_error_overlay();
    const string::dimension available{
        static_cast<std::uint16_t>(std::min<std::uint32_t>(screen_size.width, 0xFFFF)),
        static_cast<std::uint16_t>(std::min<std::uint32_t>(screen_size.height, 0xFFFF)) };
    builder_.end(available, string::text_measurer{ atlas_.get(), builder_.text_runs() });

    // Pack shapes and glyphs from the one tree into this frame's ring buffers.
    const std::vector<GpuShape> shapes = pack_shapes(builder_.nodes());
    const std::vector<GpuGlyph> glyphs = shape_glyphs(*atlas_, builder_.nodes(), builder_.text_runs());
    if ((shapes.size() > kMaxShapes || glyphs.size() > kMaxGlyphs) && !warned_overflow_)
    {
        STRING_LOG_WARN("UIPass: content exceeds ring capacity ({} shapes / {} glyphs); truncating",
                        shapes.size(), glyphs.size());
        warned_overflow_ = true;
    }
    Ring& sr = shape_ring_[current_frame];
    sr.count = static_cast<std::uint32_t>(std::min<std::size_t>(shapes.size(), kMaxShapes));
    std::memcpy(sr.mapped, shapes.data(), sr.count * sizeof(GpuShape));
    Ring& gr = glyph_ring_[current_frame];
    gr.count = static_cast<std::uint32_t>(std::min<std::size_t>(glyphs.size(), kMaxGlyphs));
    std::memcpy(gr.mapped, glyphs.data(), gr.count * sizeof(GpuGlyph));

    // Hit-test the cursor against this frame's layout for hover, and resolve focus / mode.
    const glm::vec2 mouse = input_.mouse_position();
    const string::layout_node* hit = builder_.hit_test(
        static_cast<std::uint16_t>(std::clamp(mouse.x, 0.0f, 65535.0f)),
        static_cast<std::uint16_t>(std::clamp(mouse.y, 0.0f, 65535.0f)));
    hovered_id_ = hit != nullptr ? hit->element.id.hash : 0;

    if (input_.mouse_captured())
    {
        focused_id_ = 0;  // game mode (mouse-look): nothing in the UI is focused
    }
    else if (input_.mouse_button_pressed(MouseButton::LEFT))
    {
        // UI mode: a click focuses the element under the cursor; a click on empty UI space falls
        // through to request game mode (so the UI gets first dibs on the click).
        if (hovered_id_ != 0)
        {
            focused_id_ = hovered_id_;
        }
        else
        {
            input_.set_capture_requested(true);
            focused_id_ = 0;
        }
    }
}

void UIPass::record(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    if (current_frame >= shape_ring_.size())
    {
        return;
    }
    VkCommandBuffer command_buffer = recorder.get_command_buffer();

    // Shapes first (under), then glyphs (over).
    const string::gpu::pipeline& shape_p = shape_program_->current();
    const Ring& sr = shape_ring_[current_frame];
    if (sr.count > 0)
    {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shape_p.pipeline);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            shape_p.pipeline_layout, 0, 1, &descriptor_set_, 0, nullptr);
        const ShapePush push{
            { static_cast<float>(screen_size.width), static_cast<float>(screen_size.height) }, sr.slot };
        vkCmdPushConstants(command_buffer, shape_p.pipeline_layout, shape_p.push_constants.stageFlags,
            0, sizeof(ShapePush), &push);
        vkCmdDraw(command_buffer, 6, sr.count, 0, 0);
    }

    const string::gpu::pipeline& text_p = text_program_->current();
    const Ring& gr = glyph_ring_[current_frame];
    if (gr.count > 0)
    {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, text_p.pipeline);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            text_p.pipeline_layout, 0, 1, &descriptor_set_, 0, nullptr);
        const TextPush push{
            { static_cast<float>(screen_size.width), static_cast<float>(screen_size.height) },
            gr.slot, atlas_slot_ };
        vkCmdPushConstants(command_buffer, text_p.pipeline_layout,
            text_p.push_constants.stageFlags, 0, sizeof(TextPush), &push);
        vkCmdDraw(command_buffer, 6, gr.count, 0, 0);
    }
}

}  // namespace sandbox
