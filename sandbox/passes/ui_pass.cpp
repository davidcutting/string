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
#include <string/vulkan/vulkan_utils.hpp>

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
    std::uint32_t first;   // base index of this sub-draw's range (overlay layer = tail range)
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
// Per-node "topmost layer" flags, inherited down the tree (an overlay panel's children are overlay
// too). Parents always precede children in the node pool, so one forward pass resolves inheritance.
std::vector<std::uint8_t> overlay_flags(std::span<const string::layout_node> nodes)
{
    std::vector<std::uint8_t> flags(nodes.size(), 0);
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const string::layout_node& n = nodes[i];
        flags[i] = (n.element.overlay || (!n.is_root() && flags[n.parent])) ? 1 : 0;
    }
    return flags;
}

std::vector<GpuShape> pack_shapes(std::span<const string::layout_node> nodes,
                                  std::span<const std::uint8_t> flags, bool overlay_layer)
{
    std::vector<GpuShape> shapes;
    shapes.reserve(nodes.size());
    for (std::size_t ni = 0; ni < nodes.size(); ++ni)
    {
        const string::layout_node& n = nodes[ni];
        if ((flags[ni] != 0) != overlay_layer)
        {
            continue;  // wrong layer: overlay content packs after ALL main content
        }
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
            { effective_radius(n.element, w, h), static_cast<float>(n.element.stroke_width),
              n.element.sweep / 255.0f, 0.0f },
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
    std::uint32_t first;   // base index of this sub-draw's range (overlay layer = tail range)
};

// Shape the text nodes' strings into positioned glyph quads. Decodes UTF-8 to codepoints (dynamic
// atlas: glyphs rasterise on first sight), scales the reference-size SDF metrics by font_px/bake_px,
// applies kerning, honours '\n' and word-wraps to the node's box width, and clips glyphs outside the
// node box (so scroll regions / narrow tooltips don't spill). MUST match dynamic_text_measurer so the
// laid-out box fits the drawn glyphs.
std::vector<GpuGlyph> shape_glyphs(string::dynamic_font_atlas& atlas,
                                   std::span<const string::layout_node> nodes,
                                   std::span<const string::text_run> texts,
                                   std::span<const std::uint8_t> flags, bool overlay_layer)
{
    std::vector<GpuGlyph> glyphs;
    for (std::size_t ni = 0; ni < nodes.size(); ++ni)
    {
        const string::layout_node& n = nodes[ni];
        if ((flags[ni] != 0) != overlay_layer)
        {
            continue;  // wrong layer: overlay text packs (and draws) after all main text
        }
        if (n.element.text == 0 || n.element.text >= texts.size())
        {
            continue;
        }
        const std::array<float, 4> color = to_linear(n.element.color);
        const string::text_run& run = texts[n.element.text];
        const std::string_view str = run.str;
        const float px = run.font_px > 0 ? static_cast<float>(run.font_px) : atlas.bake_px();
        const float s = px / atlas.bake_px();

        const float origin_x = static_cast<float>(n.box.x);
        const float clip_x0 = origin_x;
        const float clip_x1 = origin_x + n.box.dimension.width;
        const float clip_y0 = static_cast<float>(n.box.y);
        const float clip_y1 = clip_y0 + n.box.dimension.height;
        // Wrap contract shared with dynamic_text_measurer: word-wrap ONLY fixed-width elements
        // (to their fixed width == box width); everything else is one unwrapped line, clipped.
        // Identical algorithm + identical whole-pixel advance rounding, or wrapped text draws a
        // different line count than the measured box reserves and overdraws neighbouring rows.
        const bool do_wrap = n.element.sizing.width.mode == string::size_mode::FIXED;
        const float wrap = static_cast<float>(n.box.dimension.width);

        float pen_x = origin_x;
        float word_w = 0;  // width of the pending word (since the last break opportunity)
        float baseline_y = static_cast<float>(n.box.y) + atlas.ascent() * s;
        std::uint32_t prev = 0;
        std::size_t i = 0;
        while (i < str.size())
        {
            const std::uint32_t cp = string::utf8_next(str, i);
            if (cp == '\n')
            {
                pen_x = origin_x;
                word_w = 0;
                baseline_y += atlas.line_height() * s;
                prev = 0;
                continue;
            }
            const string::glyph_metrics& g = atlas.glyph(cp);
            const float adv = std::round((g.xadvance + atlas.kerning(prev, cp)) * s);
            if (cp == ' ')
            {
                pen_x += adv;
                word_w = 0;
                prev = cp;
                continue;
            }
            const float line_w = pen_x - origin_x;
            if (do_wrap && line_w + adv > wrap && line_w > 0 &&
                (word_w == 0 || word_w == line_w))
            {
                // word_w==0: wrap the fresh word to the next line (space-boundary wrap).
                // word_w==line_w: a single word longer than the line — hard-break it.
                pen_x = origin_x;
                word_w = 0;
                baseline_y += atlas.line_height() * s;
            }
            if (g.w > 0 && g.h > 0)
            {
                // Whole-pixel quad placement (pen is integral; snap the bearing too): consistent
                // stems and letter spacing at minified SDF sizes.
                const float gx = std::round(pen_x + g.xoff * s);
                const float gy = std::round(baseline_y + g.yoff * s);
                const float gw = g.w * s;
                const float gh = g.h * s;
                // Clip: drop glyphs fully outside the node box (scroll/tooltip containment).
                if (gx + gw > clip_x0 && gx < clip_x1 + 1.0f && gy + gh > clip_y0 && gy < clip_y1 + 1.0f)
                {
                    glyphs.push_back(GpuGlyph{
                        { gx, gy, gw, gh },
                        { g.u0, g.v0, g.u1, g.v1 },
                        { color[0], color[1], color[2], color[3] },
                    });
                }
            }
            pen_x += adv;
            word_w += adv;
            prev = cp;
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

UIPass::UIPass(PassContext& context, std::shared_ptr<string::dynamic_font_atlas> atlas, Author author)
: device_(context.device)
, allocator_(context.allocator)
, descriptor_table_(context.descriptor_table)
, atlas_(std::move(atlas))
, author_(std::move(author))
, input_(context.input)
, shader_registry_(context.shader_registry)
{
    const std::filesystem::path& resources_path = context.resources_path;

    // --- Dynamic SDF atlas: an R8 sampled image, seeded here and re-uploaded (dirty region) each
    // frame the atlas grows (record_compute). ---
    atlas_upload_bytes_ = static_cast<std::uint32_t>(atlas_->pixels_size());
    atlas_image_ = allocator_.create_resource(string::gpu::image_info{
        .extent = { atlas_->atlas_w(), atlas_->atlas_h(), 1 },
        .format = VK_FORMAT_R8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
        .mip_levels = 1,
    });
    context.transfer.upload_image(atlas_->pixels(), atlas_->pixels_size(), atlas_image_);
    atlas_->take_dirty();  // consume the seed generation's dirty flag; record_compute owns re-uploads
    atlas_uploaded_ = true;
    // Per-frame-in-flight host-visible staging buffers for the dynamic re-upload.
    atlas_staging_.resize(context.frames_in_flight);
    for (string::gpu::resource_id& s : atlas_staging_)
    {
        s = allocator_.create_resource(string::gpu::buffer_info{
            .size = atlas_upload_bytes_,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
            .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                              | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        });
    }
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
    for (string::gpu::resource_id s : atlas_staging_)
    {
        allocator_.destroy_resource(s);
    }
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

void UIPass::update(float delta_time, uint16_t current_frame)
{
    if (screen_size.width == 0 || screen_size.height == 0 || current_frame >= shape_ring_.size())
    {
        return;  // not sized yet
    }

    // A press edge in UI mode (cursor free) on the hovered element, resolved BEFORE authoring so the
    // author sees this frame's click for press feedback / activation. Focus is resolved after layout.
    // Gamepad: the A/South button activates the currently-focused element (controller-first UI).
    const bool ui_mode = !input_.mouse_captured();
    const bool click = ui_mode && input_.mouse_button_pressed(MouseButton::LEFT);
    const bool pad_activate = input_.gamepad_pressed(String::GamepadButton::A);
    const std::uint64_t pressed_this_frame =
        click ? hovered_id_ : (pad_activate ? focused_id_ : 0);
    // Feel instrumentation: stamp the click so record() can report click->record latency this frame.
    if ((click && hovered_id_ != 0) || (pad_activate && focused_id_ != 0))
    {
        click_time_ = std::chrono::steady_clock::now();
        click_pending_ = true;
    }

    // Author + lay out the whole UI. The callback adds children to a screen-filling root; it reads
    // last frame's hovered/focused ids to style, and the live input to route text to a focused field.
    const auto ui_t0 = std::chrono::steady_clock::now();
    builder_.clear();
    builder_.begin(string::format{ .padding = { 12, 12, 12, 12 }, .gap = 8,
                                   .direction = string::direction::VERTICAL });
    author_(builder_, UiContext{ input_, hovered_id_, focused_id_, delta_time, pressed_this_frame });
    author_error_overlay();
    const string::dimension available{
        static_cast<std::uint16_t>(std::min<std::uint32_t>(screen_size.width, 0xFFFF)),
        static_cast<std::uint16_t>(std::min<std::uint32_t>(screen_size.height, 0xFFFF)) };
    builder_.end(available, string::dynamic_text_measurer{ atlas_.get(), builder_.text_runs() });

    // Pack shapes and glyphs from the one tree into this frame's ring buffers: main layer first,
    // then the overlay (topmost) layer — record() draws main shapes -> main text -> overlay shapes
    // -> overlay text, so a modal surface covers the scene UI's text as well as its shapes.
    const std::vector<std::uint8_t> flags = overlay_flags(builder_.nodes());
    std::vector<GpuShape> shapes = pack_shapes(builder_.nodes(), flags, false);
    const std::size_t shapes_main = shapes.size();
    {
        const std::vector<GpuShape> over = pack_shapes(builder_.nodes(), flags, true);
        shapes.insert(shapes.end(), over.begin(), over.end());
    }
    std::vector<GpuGlyph> glyphs = shape_glyphs(*atlas_, builder_.nodes(), builder_.text_runs(),
                                                flags, false);
    const std::size_t glyphs_main = glyphs.size();
    {
        const std::vector<GpuGlyph> over =
            shape_glyphs(*atlas_, builder_.nodes(), builder_.text_runs(), flags, true);
        glyphs.insert(glyphs.end(), over.begin(), over.end());
    }
    if ((shapes.size() > kMaxShapes || glyphs.size() > kMaxGlyphs) && !warned_overflow_)
    {
        STRING_LOG_WARN("UIPass: content exceeds ring capacity ({} shapes / {} glyphs); truncating",
                        shapes.size(), glyphs.size());
        warned_overflow_ = true;
    }
    Ring& sr = shape_ring_[current_frame];
    sr.count = static_cast<std::uint32_t>(std::min<std::size_t>(shapes.size(), kMaxShapes));
    sr.split = static_cast<std::uint32_t>(std::min<std::size_t>(shapes_main, sr.count));
    std::memcpy(sr.mapped, shapes.data(), sr.count * sizeof(GpuShape));
    Ring& gr = glyph_ring_[current_frame];
    gr.count = static_cast<std::uint32_t>(std::min<std::size_t>(glyphs.size(), kMaxGlyphs));
    gr.split = static_cast<std::uint32_t>(std::min<std::size_t>(glyphs_main, gr.count));
    std::memcpy(gr.mapped, glyphs.data(), gr.count * sizeof(GpuGlyph));

    // UI CPU cost (author + layout + shape/glyph pack) — the "UI cost visible standalone" number the
    // brief asks for. Rolling avg logged every 300 frames so the 500-nameplate stress can be read off
    // the log without Tracy. STRING_PROFILE zones (brief 06) still wrap this for -Dtracy runs.
    const auto ui_us = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - ui_t0).count();
    ui_cpu_us_accum_ += ui_us;
    ui_cpu_us_peak_ = std::max<std::int64_t>(ui_cpu_us_peak_, ui_us);
    if (++ui_cpu_samples_ >= 300)
    {
        STRING_LOG_INFO("[ui-cpu] avg {:.3f} ms, peak {:.3f} ms ({} shapes / {} glyphs)",
                        (ui_cpu_us_accum_ / 300.0) / 1000.0, ui_cpu_us_peak_ / 1000.0,
                        sr.count, gr.count);
        ui_cpu_us_accum_ = 0;
        ui_cpu_us_peak_ = 0;
        ui_cpu_samples_ = 0;
    }

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

    // --- Gamepad directional focus navigation (brief 05: controller-first) ---
    // On a d-pad / left-stick edge, move focus to the nearest focusable node (non-zero id) in that
    // direction, scored by directional distance + lateral penalty. Also enters UI mode on any nav so
    // a controller can drive the UI without touching the mouse.
    int nav_x = 0, nav_y = 0;
    if (input_.gamepad_pressed(String::GamepadButton::LEFT))  nav_x = -1;
    if (input_.gamepad_pressed(String::GamepadButton::RIGHT)) nav_x = 1;
    if (input_.gamepad_pressed(String::GamepadButton::UP))    nav_y = -1;
    if (input_.gamepad_pressed(String::GamepadButton::DOWN))  nav_y = 1;
    // Left-stick flick (rearmed when it recenters) so a stick can navigate like a d-pad.
    const float sx = input_.gamepad_axis(String::GamepadAxis::LEFT_X);
    const float sy = input_.gamepad_axis(String::GamepadAxis::LEFT_Y);
    constexpr float kStick = 0.6f;
    if (nav_x == 0 && nav_y == 0 && stick_armed_)
    {
        if (sx < -kStick) nav_x = -1;
        else if (sx > kStick) nav_x = 1;
        else if (sy < -kStick) nav_y = -1;
        else if (sy > kStick) nav_y = 1;
        if (nav_x != 0 || nav_y != 0) stick_armed_ = false;
    }
    if (std::abs(sx) < 0.3f && std::abs(sy) < 0.3f)
        stick_armed_ = true;

    if (nav_x != 0 || nav_y != 0)
    {
        input_.set_capture_requested(false);  // any nav intent means the player wants the UI
        // Current focus centre (or screen centre if nothing focused yet).
        const string::layout_node* cur = focused_id_ != 0 ? builder_.find(focused_id_) : nullptr;
        float cx = cur ? cur->box.x + cur->box.dimension.width * 0.5f : screen_size.width * 0.5f;
        float cy = cur ? cur->box.y + cur->box.dimension.height * 0.5f : screen_size.height * 0.5f;

        std::uint64_t best = 0;
        float best_score = 1e18f;
        for (const string::layout_node& n : builder_.nodes())
        {
            if (n.element.id.hash == 0 || n.element.id.hash == focused_id_)
                continue;  // only focusable (id'd) nodes; skip self
            const float nx = n.box.x + n.box.dimension.width * 0.5f;
            const float ny = n.box.y + n.box.dimension.height * 0.5f;
            const float dx = nx - cx;
            const float dy = ny - cy;
            // Must lie in the requested half-plane.
            const float along = dx * nav_x + dy * nav_y;
            if (along <= 1.0f)
                continue;
            const float lateral = std::abs(dx * nav_y) + std::abs(dy * nav_x);
            const float score = along + lateral * 2.0f;  // prefer aligned + close
            if (score < best_score)
            {
                best_score = score;
                best = n.element.id.hash;
            }
        }
        if (best != 0)
            focused_id_ = best;
    }
}

bool UIPass::record_compute(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    // Re-upload the dynamic atlas when new glyphs appeared this frame (the author's measure/shape
    // rasterised them into the CPU atlas during update()). Runs OUTSIDE dynamic rendering, so the
    // image-layout barriers are legal here. Whole-atlas copy (simple + the atlas is ~1MB); the
    // dirty flag makes it a no-op on the common no-new-glyph frame.
    if (current_frame >= atlas_staging_.size() || !atlas_->take_dirty())
    {
        return false;
    }

    const string::gpu::allocated_buffer& staging = allocator_.get_buffer(atlas_staging_[current_frame]);
    std::memcpy(staging.allocation_info.pMappedData, atlas_->pixels(), atlas_upload_bytes_);

    VkCommandBuffer cb = recorder.get_command_buffer();
    const VkImage image = allocator_.get_image(atlas_image_).image;

    // SHADER_READ (or UNDEFINED, but we uploaded the seed) -> TRANSFER_DST.
    String::vku::transition_image(cb, {
        .image = image,
        .old_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .new_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .src_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dst_stage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dst_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
    });

    const VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { atlas_->atlas_w(), atlas_->atlas_h(), 1 },
    };
    vkCmdCopyBufferToImage(cb, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // TRANSFER_DST -> SHADER_READ for the color pass's text draw.
    String::vku::transition_image(cb, {
        .image = image,
        .old_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
    });
    return true;
}

void UIPass::record(string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    if (current_frame >= shape_ring_.size())
    {
        return;
    }
    VkCommandBuffer command_buffer = recorder.get_command_buffer();

    // Feel instrumentation (M5): a click authored this frame is recorded this frame (immediate mode),
    // so this is the CPU-side click->visible-response latency. GPU present adds ~1 frame on top.
    if (click_pending_)
    {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - click_time_).count();
        STRING_LOG_INFO("UI feel: click->record latency {} us ({:.2f} ms) [+~1 frame present]",
                        us, us / 1000.0);
        click_pending_ = false;
    }

    // Layered draw order: main shapes -> main text -> overlay shapes -> overlay text. Shapes and
    // glyphs are separate streams, so the overlay layer (debug console/HUD/modal surfaces) must
    // re-run both pipelines after the main layer to cover the scene UI's text as well as shapes.
    const string::gpu::pipeline& shape_p = shape_program_->current();
    const string::gpu::pipeline& text_p = text_program_->current();
    const Ring& sr = shape_ring_[current_frame];
    const Ring& gr = glyph_ring_[current_frame];

    const auto draw_shapes = [&](std::uint32_t first, std::uint32_t count) {
        if (count == 0) return;
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shape_p.pipeline);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            shape_p.pipeline_layout, 0, 1, &descriptor_set_, 0, nullptr);
        const ShapePush push{
            { static_cast<float>(screen_size.width), static_cast<float>(screen_size.height) },
            sr.slot, first };
        vkCmdPushConstants(command_buffer, shape_p.pipeline_layout, shape_p.push_constants.stageFlags,
            0, sizeof(ShapePush), &push);
        vkCmdDraw(command_buffer, 6, count, 0, 0);
    };
    const auto draw_text = [&](std::uint32_t first, std::uint32_t count) {
        if (count == 0) return;
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, text_p.pipeline);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            text_p.pipeline_layout, 0, 1, &descriptor_set_, 0, nullptr);
        const TextPush push{
            { static_cast<float>(screen_size.width), static_cast<float>(screen_size.height) },
            gr.slot, atlas_slot_, first };
        vkCmdPushConstants(command_buffer, text_p.pipeline_layout,
            text_p.push_constants.stageFlags, 0, sizeof(TextPush), &push);
        vkCmdDraw(command_buffer, 6, count, 0, 0);
    };

    draw_shapes(0, sr.split);
    draw_text(0, gr.split);
    draw_shapes(sr.split, sr.count - sr.split);
    draw_text(gr.split, gr.count - gr.split);
}

}  // namespace sandbox
