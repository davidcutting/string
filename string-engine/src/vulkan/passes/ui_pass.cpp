#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <vector>

#include <string/vulkan/passes/ui_pass.hpp>
#include <string/vulkan/pipeline_builder.hpp>
#include <string/core/layout.hpp>

namespace String
{
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

// Build a simple static demo UI (a padded panel with three colored boxes) and pack the
// laid-out nodes into GPU shapes. This is where core/layout.hpp (namespace `string`) meets
// the renderer (namespace `String`).
std::vector<GpuShape> build_ui_shapes()
{
    using namespace string;

    element panel{};
    panel.color = { 30, 30, 46, 220 };
    panel.stroke_color = { 88, 91, 112, 255 };   // subtle surface border
    panel.stroke_width = 2;
    panel.radius = 16;
    panel.shape = shape::ROUNDED_RECTANGLE;
    panel.sizing = size_fit();   // shrink-wrap the children + padding

    // a) sharp rectangle, no border
    element rect_box{};
    rect_box.color = { 243, 139, 168, 255 };
    rect_box.shape = shape::RECTANGLE;
    rect_box.sizing = size_fixed(180, 60);

    // b) rounded rectangle with a border
    element rounded_box{};
    rounded_box.color = { 166, 227, 161, 255 };
    rounded_box.stroke_color = { 64, 120, 80, 255 };
    rounded_box.stroke_width = 3;
    rounded_box.radius = 12;
    rounded_box.shape = shape::ROUNDED_RECTANGLE;
    rounded_box.sizing = size_fixed(180, 60);

    // c) circle with a border
    element circle{};
    circle.color = { 137, 180, 250, 255 };
    circle.stroke_color = { 40, 60, 110, 255 };
    circle.stroke_width = 2;
    circle.shape = shape::CIRCLE;
    circle.sizing = size_fixed(60, 60);

    layout_builder b;
    b.begin(panel, format{ .padding = { 8, 8, 8, 8 }, .gap = 8, .direction = direction::VERTICAL })
         .add_element(rect_box)
         .add_element(rounded_box)
         .add_element(circle)
     .end();

    const auto to_linear = [](color c) {
        return std::array<float, 4>{ srgb_to_linear(c.r / 255.0f), srgb_to_linear(c.g / 255.0f),
                                     srgb_to_linear(c.b / 255.0f), c.a / 255.0f };
    };

    std::vector<GpuShape> shapes;
    shapes.reserve(b.nodes().size());
    for (const auto& n : b.nodes())
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

UIPass::UIPass(Device& device, ResourceAllocator& allocator, DescriptorTable& descriptor_table,
               const std::filesystem::path& resources_path, const uint16_t& frames_in_flight)
: device_(device)
, allocator_(allocator)
, descriptor_table_(descriptor_table)
{
    (void)frames_in_flight;

    // Build the layout once and upload its shapes to a persistent, host-visible storage
    // buffer (static UI: no per-frame ring needed yet).
    const std::vector<GpuShape> shapes = build_ui_shapes();
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

    const VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(UIPush),
    };
    pipeline_layout_ = PipelineLayoutBuilder()
        .set_descriptor_set_layout({ descriptor_table.get_layout() })
        .set_push_constant_ranges({ push_constant_range })
        .build(device_);

    // Overlay: procedural quads (no vertex input), alpha-blended, no depth test (but declares
    // the offscreen D32 format so the pipeline matches the pass), offscreen R16F target.
    pipeline_ = PipelineBuilder(device_)
        .add_vertex_shader(resources_path / "shaders/ui_shader.vert.spv")
        .add_fragment_shader(resources_path / "shaders/ui_shader.frag.spv")
        .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .set_tessellation()
        .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .set_multisampling()
        .enable_depth_stencil(false, false)
        .enable_color_blending()
        .build_graphics_pipeline(pipeline_layout_);
}

UIPass::~UIPass()
{
    vkDestroyPipeline(device_.get_device(), pipeline_, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), pipeline_layout_, nullptr);
    descriptor_table_.unbind(shape_buffer_, DescriptorType::STORAGE_BUFFER);
    allocator_.destroy_resource(shape_buffer_);
}

void UIPass::record(CommandRecorder& recorder, const uint16_t& current_frame)
{
    (void)current_frame;
    if (shape_count_ == 0)
    {
        return;
    }

    VkCommandBuffer command_buffer = recorder.get_command_buffer();

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_layout_, 0, 1, &descriptor_set_, 0, nullptr);

    const UIPush push{
        { static_cast<float>(screen_size.width), static_cast<float>(screen_size.height) },
        shape_slot_,
    };
    vkCmdPushConstants(command_buffer, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
        0, sizeof(UIPush), &push);

    // 6 verts (two triangles) per shape, one instance per shape.
    vkCmdDraw(command_buffer, 6, shape_count_, 0, 0);
}

}
