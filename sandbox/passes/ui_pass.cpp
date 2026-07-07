#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <span>
#include <vector>

#include "ui_pass.hpp"
#include <string/vulkan/pipeline_builder.hpp>
#include <string/core/layout.hpp>

namespace sandbox
{
using namespace String;
namespace
{

// GPU shape, std430-compatible: matches `struct Shape` in shaders/ui_shader.vert. All-vec4
// so the layout is unambiguous (16-byte aligned, 64-byte stride).
//   rect   = { pos.x, pos.y, size.x, size.y }  (pixels)
//   fill   = { r, g, b, a }                    (LINEAR — see srgb_to_linear)
//   stroke = { r, g, b, a }                    (LINEAR border colour)
//   params = { corner_radius, stroke_width, _, _ }  (pixels)
struct GpuShape
{
    float rect[4];
    float fill[4];
    float stroke[4];
    float params[4];
};

// Authored UI colors are sRGB, but they're written into the linear HDR offscreen (which the
// composite then re-encodes to sRGB) — so decode to linear on the way in, or they brighten.
// Alpha is already linear and left alone.
float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Honor the shape enum by resolving it to a corner radius the SDF can render directly:
// RECTANGLE is sharp, ROUNDED_RECTANGLE uses `radius`, CIRCLE fills to half the smaller side
// (a perfect circle for a square element, a pill otherwise). The shader stays shape-agnostic.
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

struct UIPush
{
    float screen_size[2];
    uint32_t shape_slot;
};

// Pack already-laid-out nodes into GPU shapes. This is the library's half of the UI: the
// application authored the layout (which elements, colors, shapes); here each node becomes a
// std430 GpuShape with linearized colors and a shape-resolved corner radius. This is where
// core/layout.hpp (namespace `string`) meets the renderer (namespace `String`).
std::vector<GpuShape> pack_shapes(std::span<const string::layout_node> nodes)
{
    using namespace string;

    const auto to_linear = [](color c) {
        return std::array<float, 4>{ srgb_to_linear(c.r / 255.0f), srgb_to_linear(c.g / 255.0f),
                                     srgb_to_linear(c.b / 255.0f), c.a / 255.0f };
    };

    std::vector<GpuShape> shapes;
    shapes.reserve(nodes.size());
    for (const auto& n : nodes)
    {
        const float w = static_cast<float>(n.box.dimension.width);
        const float h = static_cast<float>(n.box.dimension.height);
        const auto fill = to_linear(n.element.color);
        const auto stroke = to_linear(n.element.stroke_color);
        shapes.push_back(GpuShape{
            { static_cast<float>(n.box.x), static_cast<float>(n.box.y), w, h },
            { fill[0], fill[1], fill[2], fill[3] },
            { stroke[0], stroke[1], stroke[2], stroke[3] },
            { effective_radius(n.element, w, h), static_cast<float>(n.element.stroke_width), 0.0f, 0.0f },
        });
    }
    return shapes;
}

}  // namespace

UIPass::UIPass(PassContext& context, std::vector<string::layout_node> nodes)
: device_(context.device)
, allocator_(context.allocator)
, descriptor_table_(context.descriptor_table)
{
    const std::filesystem::path& resources_path = context.resources_path;

    // Pack the application-authored layout and upload it to a persistent, host-visible storage
    // buffer (static UI: no per-frame ring needed yet).
    const std::vector<GpuShape> shapes = pack_shapes(nodes);
    shape_count_ = static_cast<uint32_t>(shapes.size());

    shape_buffer_ = allocator_.create_resource(BufferInfo{
        .size = shape_count_ * sizeof(GpuShape),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
        .allocation_flags = {},
    });
    allocator_.copy_data_to_buffer(shapes.data(), shape_buffer_);

    descriptor_table_.bind(shape_buffer_, DescriptorType::STORAGE_BUFFER);
    descriptor_set_ = descriptor_table_.get_set();
    shape_slot_ = descriptor_table_.get_binding_slot(shape_buffer_, DescriptorType::STORAGE_BUFFER);

    // Declare the storage buffer this pass reads (vertex shader) + the color target it draws
    // into (overlay; no depth), for the render graph.
    usages = {
        { shape_buffer_,        Access::StorageRead, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT },
        { context.color_target, Access::ColorWrite,  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT },
    };

    const VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(UIPush),
    };
    pipeline_.pipeline_layout = PipelineLayoutBuilder()
        .set_descriptor_set_layout({ descriptor_table_.get_layout() })
        .set_push_constant_ranges({ push_constant_range })
        .build(device_);

    // Overlay: procedural quads (no vertex input), alpha-blended, no depth test (but declares
    // the offscreen D32 format so the pipeline matches the pass), offscreen R16F target.
    pipeline_.pipeline = PipelineBuilder(device_)
        .add_vertex_shader(resources_path / "shaders/ui_shader.vert.spv")
        .add_fragment_shader(resources_path / "shaders/ui_shader.frag.spv")
        .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .set_tessellation()
        .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .set_multisampling()
        .enable_depth_stencil(false, false)
        .enable_color_blending()
        .build_graphics_pipeline(pipeline_.pipeline_layout);
    pipeline_.pipeline_type = PipelineType::GRAPHICS;
}

UIPass::~UIPass()
{
    vkDestroyPipeline(device_.get_device(), pipeline_.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), pipeline_.pipeline_layout, nullptr);
    descriptor_table_.unbind(shape_buffer_, DescriptorType::STORAGE_BUFFER);
    allocator_.destroy_resource(shape_buffer_);
}

void UIPass::record(CommandRecorder& recorder, uint16_t current_frame)
{
    (void)current_frame;
    if (shape_count_ == 0)
    {
        return;
    }

    VkCommandBuffer command_buffer = recorder.get_command_buffer();

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_.pipeline_layout, 0, 1, &descriptor_set_, 0, nullptr);

    const UIPush push{
        { static_cast<float>(screen_size.width), static_cast<float>(screen_size.height) },
        shape_slot_,
    };
    vkCmdPushConstants(command_buffer, pipeline_.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
        0, sizeof(UIPush), &push);

    // 6 verts (two triangles) per shape, one instance per shape.
    vkCmdDraw(command_buffer, 6, shape_count_, 0, 0);
}

}
