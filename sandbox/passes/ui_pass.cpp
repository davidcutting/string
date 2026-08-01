#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ui_pass.hpp"
#include "overlay_common.hpp"
#include "debug_cvars.hpp"

#include <string/core/layout_dump.hpp>
#include <string/core/logger.hpp>
#include <string/core/text_measurer.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/vulkan/passes/composite_pass.hpp>
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
    float inv_exposure;    // cancels the composite's EV100 exposure (display-referred UI)
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

// Per-node DRAW-BATCH KEY: (overlay << 8) | z, both inherited from the nearest ancestor that
// declares them. Nodes are in pre-order so a parent is always resolved before its children.
//
// This is the single ordering authority for the UI: record() draws each distinct key's shapes AND
// text before moving to the next, and `string::ui::hit_test_layered` compares the same key. Two
// draw streams (shapes, glyphs) is exactly why the key has to exist — without batching per key, all
// shapes precede all text, so a lower panel's TEXT lands on top of a raised panel's BACKGROUND.
std::vector<std::uint16_t> layer_keys(std::span<const string::layout_node> nodes)
{
    std::vector<std::uint16_t> keys(nodes.size(), 0);
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const string::layout_node& n = nodes[i];
        const std::uint16_t inherited = n.is_root() ? 0 : keys[n.parent];
        const std::uint16_t overlay = (n.element.overlay || (inherited & 0x100)) ? 0x100 : 0;
        // z is inherited only when this node does not declare its own.
        const std::uint16_t z = n.element.z != 0 ? n.element.z : (inherited & 0xFF);
        keys[i] = static_cast<std::uint16_t>(overlay | z);
    }
    return keys;
}

std::vector<GpuShape> pack_shapes(std::span<const string::layout_node> nodes,
                                  std::span<const std::uint16_t> keys, std::uint16_t layer)
{
    std::vector<GpuShape> shapes;
    shapes.reserve(nodes.size());
    for (std::size_t ni = 0; ni < nodes.size(); ++ni)
    {
        const string::layout_node& n = nodes[ni];
        if (keys[ni] != layer)
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
    float inv_exposure;    // cancels the composite's EV100 exposure (display-referred UI)
};

// Shape the text nodes' strings into positioned glyph quads. Decodes UTF-8 to codepoints (dynamic
// atlas: glyphs rasterise on first sight), scales the reference-size SDF metrics by font_px/bake_px,
// applies kerning, honours '\n' and word-wraps to the node's box width, and clips glyphs outside the
// node box (so scroll regions / narrow tooltips don't spill). MUST match dynamic_text_measurer so the
// laid-out box fits the drawn glyphs.
std::vector<GpuGlyph> shape_glyphs(string::dynamic_font_atlas& atlas,
                                   std::span<const string::layout_node> nodes,
                                   std::span<const string::text_run> texts,
                                   std::span<const std::uint16_t> keys, std::uint16_t layer)
{
    std::vector<GpuGlyph> glyphs;
    for (std::size_t ni = 0; ni < nodes.size(); ++ni)
    {
        const string::layout_node& n = nodes[ni];
        if (keys[ni] != layer)
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

UIPass::UIPass(engine_context& context, std::shared_ptr<string::dynamic_font_atlas> atlas,
               Author author, PostLayout post_layout)
: device_(context.device)
, allocator_(context.allocator)
, descriptor_table_(context.descriptor_table)
, atlas_(std::move(atlas))
, author_(std::move(author))
, post_layout_(std::move(post_layout))
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
    batches_.resize(context.frames_in_flight);
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

// THE ONE CROSSING (brief 12 M0b). Translates platform input into engine UI vocabulary — the only
// place in the codebase that does. Host conventions live HERE, not in the engine: what counts as a
// stick flick, which gamepad button activates, whether the cursor is captured. The engine gets
// de-edged intent and nothing else, which is what keeps `string::ui` platform-free and testable.
string::ui::interaction_input UIPass::populate_interaction(float delta_time)
{
    string::ui::interaction_input in;
    const glm::vec2 mouse = input_.mouse_position();
    in.cursor_x = mouse.x;
    in.cursor_y = mouse.y;
    in.ui_mode = !input_.mouse_captured();
    in.primary_down = input_.mouse_button_down(MouseButton::LEFT);
    in.primary_pressed = input_.mouse_button_pressed(MouseButton::LEFT);
    in.primary_released = prev_click_ && !in.primary_down;
    prev_click_ = in.primary_down;
    in.activate = input_.gamepad_pressed(String::GamepadButton::A);
    in.dt = delta_time;
    in.screen = { static_cast<std::uint16_t>(std::min<std::uint32_t>(screen_size.width, 0xFFFF)),
                  static_cast<std::uint16_t>(std::min<std::uint32_t>(screen_size.height, 0xFFFF)) };

    // Directional focus-nav intent: d-pad, or a left-stick flick rearmed when the stick recentres
    // (so a held stick navigates once, like a d-pad). De-edging is the host's job.
    if (input_.gamepad_pressed(String::GamepadButton::LEFT))  in.nav_x = -1;
    if (input_.gamepad_pressed(String::GamepadButton::RIGHT)) in.nav_x = 1;
    if (input_.gamepad_pressed(String::GamepadButton::UP))    in.nav_y = -1;
    if (input_.gamepad_pressed(String::GamepadButton::DOWN))  in.nav_y = 1;
    const float sx = input_.gamepad_axis(String::GamepadAxis::LEFT_X);
    const float sy = input_.gamepad_axis(String::GamepadAxis::LEFT_Y);
    constexpr float kStick = 0.6f;
    if (in.nav_x == 0 && in.nav_y == 0 && stick_armed_)
    {
        if (sx < -kStick) in.nav_x = -1;
        else if (sx > kStick) in.nav_x = 1;
        else if (sy < -kStick) in.nav_y = -1;
        else if (sy > kStick) in.nav_y = 1;
        if (in.nav_x != 0 || in.nav_y != 0) stick_armed_ = false;
    }
    if (std::abs(sx) < 0.3f && std::abs(sy) < 0.3f)
        stick_armed_ = true;

    return in;
}

// The engine REQUESTS a mode; the host grants it. Kept out of the resolvers deliberately: capture is
// a platform decision, and `string::ui` must not know that a window or a cursor exists.
void UIPass::apply_mode_requests()
{
    if (interaction_.wants_game_mode) input_.set_capture_requested(true);
    if (interaction_.wants_ui_mode) input_.set_capture_requested(false);
}

// Brief 12 M0a. Writes the positioned tree once, on the configured frame, when dbg.ui.dump names a
// path. Deliberately dumb: no exit, no capture coupling — the harness bounds the run with `timeout`
// exactly like tools/capture.sh does, which keeps this out of the renderer's shutdown path.
void UIPass::maybe_dump_layout()
{
    const std::string& path = cv_ui_dump().get();
    if (path.empty()) return;

    const std::uint64_t want = static_cast<std::uint64_t>(std::max(0, cv_ui_dump_frame().get()));
    if (dump_written_ || ui_frames_ != want) return;
    dump_written_ = true;

    // The label carries the screen so a dump is self-identifying once it is sitting in a directory
    // of them; it lives in the header only, never in the node lines.
    std::string label = cv_ui_screen().get();
    label += " frame=";
    label += std::to_string(ui_frames_);

    const std::string text = string::dump_layout(builder_, label);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        STRING_LOG_WARN("[ui-dump] could not open '{}'", path);
        return;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    STRING_LOG_INFO("[ui-dump] wrote {} ({} nodes, frame {})", path, builder_.nodes().size(),
                    ui_frames_);
}

void UIPass::update(float delta_time, uint16_t current_frame)
{
    if (screen_size.width == 0 || screen_size.height == 0 || current_frame >= shape_ring_.size())
    {
        return;  // not sized yet
    }

    // Interaction, part 1 (pre-author): resolve this frame's press/activation from LAST frame's
    // hover/focus, and advance drag state — so the author sees the click on the frame it happens
    // (press feedback) even though hover can only be resolved against a laid-out tree. The rules
    // live in the engine (string::ui); this pass only supplies the raw signals.
    const string::ui::interaction_input in = populate_interaction(delta_time);
    string::ui::begin_interaction(interaction_, in);
    // Feel instrumentation: stamp the click so record() can report click->record latency this frame.
    if (interaction_.pressed != 0)
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
    author_(builder_, UiContext{ input_, interaction_ });
    author_error_overlay();
    const string::dimension available{
        static_cast<std::uint16_t>(std::min<std::uint32_t>(screen_size.width, 0xFFFF)),
        static_cast<std::uint16_t>(std::min<std::uint32_t>(screen_size.height, 0xFFFF)) };
    builder_.end(available, string::dynamic_text_measurer{ atlas_.get(), builder_.text_runs() });

    // The one post-layout seam (brief 12 M2c): resolved sizes exist only now, and a workspace needs
    // them to convert a splitter drag into a sizing change. Reads only.
    if (post_layout_) post_layout_(builder_);

    // Brief 12 M0a — the layout-tree dump gate. Taken HERE: after layout resolves (so boxes are
    // final) and before packing (which is a lossy projection onto the GPU rings). One dump per run,
    // on a fixed frame, so the harness can diff two runs across a facade change.
    maybe_dump_layout();
    ++ui_frames_;

    // Pack shapes and glyphs from the one tree into this frame's ring buffers, ONE BATCH PER
    // DISTINCT LAYER KEY in ascending order (main content first, then each overlay z). record()
    // draws a batch's shapes then its text before moving to the next, so a raised panel's
    // background covers a lower panel's TEXT — which two globally-ordered draw streams cannot do.
    //
    // Almost every UI has exactly two keys (main + overlay z=0), which reproduces the previous
    // shapes/text/shapes/text sequence exactly; z only costs draw calls when it is actually used.
    const std::vector<std::uint16_t> keys = layer_keys(builder_.nodes());
    std::vector<std::uint16_t> layers(keys.begin(), keys.end());
    std::sort(layers.begin(), layers.end());
    layers.erase(std::unique(layers.begin(), layers.end()), layers.end());

    std::vector<GpuShape> shapes;
    std::vector<GpuGlyph> glyphs;
    std::vector<DrawBatch>& batches = batches_[current_frame];
    batches.clear();
    for (const std::uint16_t layer : layers)
    {
        const std::vector<GpuShape> s = pack_shapes(builder_.nodes(), keys, layer);
        const std::vector<GpuGlyph> g =
            shape_glyphs(*atlas_, builder_.nodes(), builder_.text_runs(), keys, layer);
        if (s.empty() && g.empty())
            continue;
        batches.push_back(DrawBatch{ static_cast<std::uint32_t>(shapes.size()),
                                     static_cast<std::uint32_t>(s.size()),
                                     static_cast<std::uint32_t>(glyphs.size()),
                                     static_cast<std::uint32_t>(g.size()) });
        shapes.insert(shapes.end(), s.begin(), s.end());
        glyphs.insert(glyphs.end(), g.begin(), g.end());
    }
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

    // Clamp the batch ranges to what actually fit. Without this an overflowing frame would draw
    // from indices past the ring's contents — truncation must drop whole tail batches, not read
    // stale memory.
    for (DrawBatch& b : batches)
    {
        b.shape_count = b.shape_first >= sr.count
                            ? 0u : std::min(b.shape_count, sr.count - b.shape_first);
        b.glyph_count = b.glyph_first >= gr.count
                            ? 0u : std::min(b.glyph_count, gr.count - b.glyph_first);
    }

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

    // Interaction, part 2 (post-layout): hover needs a laid-out tree. Hit-test, focus resolution
    // and gamepad focus-nav are ALL engine rules now (string::ui) — deleted from this pass rather
    // than wrapped. The pass keeps only what is genuinely the host's: producing the raw signals
    // above, and applying the mode requests below (capture is a platform decision).
    string::ui::resolve_interaction(interaction_, in, builder_);
    apply_mode_requests();
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

    VkCommandBuffer cb = recorder.vk();   // escape: vku::transition_image below takes a raw cb
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
    recorder.copy_buffer_to_image(staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

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

    // The UI draws display-referred colors into the scene-referred HDR target; the composite will
    // multiply the whole frame by the EV100 exposure scale before the tonemap LUT. Pre-divide the
    // UI colors by that same scale (same frame, same value the composite's record() reads) so the
    // exposure cancels and the console/HUD keep their authored brightness at any scene EV.
    const float inv_exposure = 1.0f / String::CompositePass::exposure_scale();

    const auto draw_shapes = [&](std::uint32_t first, std::uint32_t count) {
        if (count == 0) return;
        recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, shape_p.pipeline);
        recorder.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_GRAPHICS,
            shape_p.pipeline_layout, 0, 1, &descriptor_set_, 0, nullptr);
        const ShapePush push{
            { static_cast<float>(screen_size.width), static_cast<float>(screen_size.height) },
            sr.slot, first, inv_exposure };
        recorder.push_constants(shape_p.pipeline_layout, shape_p.push_constants.stageFlags,
            0, sizeof(ShapePush), &push);
        recorder.draw(6, count, 0, 0);
    };
    const auto draw_text = [&](std::uint32_t first, std::uint32_t count) {
        if (count == 0) return;
        recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, text_p.pipeline);
        recorder.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_GRAPHICS,
            text_p.pipeline_layout, 0, 1, &descriptor_set_, 0, nullptr);
        const TextPush push{
            { static_cast<float>(screen_size.width), static_cast<float>(screen_size.height) },
            gr.slot, atlas_slot_, first, inv_exposure };
        recorder.push_constants(text_p.pipeline_layout,
            text_p.push_constants.stageFlags, 0, sizeof(TextPush), &push);
        recorder.draw(6, count, 0, 0);
    };

    // One shapes-then-text pair per layer, in ascending key order. This is what makes a raised
    // panel cover a lower panel's text as well as its background.
    for (const DrawBatch& b : batches_[current_frame])
    {
        draw_shapes(b.shape_first, b.shape_count);
        draw_text(b.glyph_first, b.glyph_count);
    }
}

}  // namespace sandbox
