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

#include <string/render/ui_pass.hpp>
#include <string/render/overlay_common.hpp>
#include <string/render/render_cvars.hpp>

#include <string/ui/layout_dump.hpp>
#include <string/core/logger.hpp>
#include <string/ui/text_measurer.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/vulkan/passes/composite_pass.hpp>
#include <string/vulkan/vulkan_utils.hpp>

namespace string::render
{
using namespace String;

// The UI's context on the engine input stack. Pushed once and left there: the UI is always present
// as a potential claimant, and WHAT it claims is decided per-surface, not by this being on or off.
static constexpr ::String::ActionId kUiContext = ::String::action_id("ctx.ui");

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
float effective_radius(const ::string::element& e, float w, float h)
{
    switch (e.shape)
    {
        case ::string::shape::RECTANGLE:         return 0.0f;
        case ::string::shape::ROUNDED_RECTANGLE: return static_cast<float>(e.radius);
        case ::string::shape::CIRCLE:            return std::min(w, h) * 0.5f;
    }
    return 0.0f;
}

// Per-node DRAW-BATCH KEY: (overlay << 8) | z, both inherited from the nearest ancestor that
// declares them. Nodes are in pre-order so a parent is always resolved before its children.
//
// This is the single ordering authority for the UI: record() draws each distinct key's shapes AND
// text before moving to the next, and `::string::ui::hit_test_layered` compares the same key. Two
// draw streams (shapes, glyphs) is exactly why the key has to exist — without batching per key, all
// shapes precede all text, so a lower panel's TEXT lands on top of a raised panel's BACKGROUND.
std::vector<std::uint16_t> layer_keys(std::span<const ::string::layout_node> nodes)
{
    std::vector<std::uint16_t> keys(nodes.size(), 0);
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const ::string::layout_node& n = nodes[i];
        const std::uint16_t inherited = n.is_root() ? 0 : keys[n.parent];
        // A subtree cannot sink below the layer it was placed on, so the MAX of declared and
        // inherited wins — same rule the single overlay bit had, generalised to an ordered list.
        const std::uint16_t declared = static_cast<std::uint16_t>(n.element.layer) << 8;
        const std::uint16_t layer = std::max<std::uint16_t>(declared, inherited & 0xFF00);
        // z is inherited only when this node does not declare its own.
        const std::uint16_t z = n.element.z != 0 ? n.element.z : (inherited & 0xFF);
        keys[i] = static_cast<std::uint16_t>(layer | z);
    }
    return keys;
}

// The clip region a node draws within: the intersection of every `clip` ancestor's box. An empty
// region means the node is entirely scrolled/panned out and draws nothing.
struct clip_rect
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool operator==(const clip_rect&) const = default;
    [[nodiscard]] bool empty() const { return x1 <= x0 || y1 <= y0; }
};

std::vector<clip_rect> clip_rects(const ::string::layout_builder& builder, ::string::dimension screen)
{
    const std::span<const ::string::layout_node> nodes = builder.nodes();
    const clip_rect full{ 0, 0, screen.width, screen.height };
    std::vector<clip_rect> out(nodes.size(), full);
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const ::string::layout_node& n = nodes[i];
        clip_rect c = n.is_root() ? full : out[n.parent];
        if (n.element.clip)
        {
            // Screen space: a scissor rect names a framebuffer region, so a clipping node inside a
            // placed surface has to be resolved through that placement first.
            const ::string::bounding_box b = builder.screen_box(n);
            // INTERSECT, never replace: a clipped child of a clipped parent shows only where both
            // allow, or an inner view could paint outside the outer one that contains it.
            c.x0 = std::max(c.x0, static_cast<int>(b.x));
            c.y0 = std::max(c.y0, static_cast<int>(b.y));
            c.x1 = std::min(c.x1, static_cast<int>(b.x) + b.dimension.width);
            c.y1 = std::min(c.y1, static_cast<int>(b.y) + b.dimension.height);
        }
        out[i] = c;
    }
    return out;
}

// One node -> one shape. Per-node rather than per-layer now that batching is run-length over CLIP
// REGIONS as well as layers: a batch ends wherever the clip changes, which can be mid-layer, so the
// packer has to be able to stop anywhere.
bool shape_of(const ::string::layout_node& n, const ::string::bounding_box& box, GpuShape& out)
{
    if (n.element.text != 0) return false;   // text node -> glyphs, not a rect
    {
        const float w = static_cast<float>(box.dimension.width);
        const float h = static_cast<float>(box.dimension.height);
        const std::array<float, 4> fill = to_linear(n.element.color);
        const std::array<float, 4> stroke = to_linear(n.element.stroke_color);
        out = GpuShape{
            { static_cast<float>(box.x), static_cast<float>(box.y), w, h },
            { fill[0], fill[1], fill[2], fill[3] },
            { stroke[0], stroke[1], stroke[2], stroke[3] },
            { effective_radius(n.element, w, h), static_cast<float>(n.element.stroke_width),
              n.element.sweep / 255.0f, 0.0f },
        };
    }
    return true;
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

// --- Images (brief 14 M4): a texture blit with inspection controls ----------------------------
struct GpuImage
{
    float rect[4];              // pos.xy, size.xy (px)
    float range[4];             // range_min, range_max, mip, false_colour
    std::uint32_t params[4];    // texture slot, channel mask, _, _
    float tint[4];              // linear rgba multiplier
};
struct ImagePush
{
    float screen_size[2];
    std::uint32_t image_slot;
    std::uint32_t first;
    float inv_exposure;
};

// One image element -> one instance. `atlas_slot` is the only source this resolves today: the widget
// was built against the glyph atlas precisely because it is already a bindless sampled texture with
// a stable slot, so the whole pipeline could be proven with no render-target barrier risk. Render
// targets need a dynamically declared SampledRead on the selected target and are the NEXT step.
void append_image(const ::string::layout_node& n, const ::string::bounding_box& box,
                  std::span<const ::string::image_run> runs,
                  std::uint32_t atlas_slot, std::vector<GpuImage>& out)
{
    if (n.element.image == 0 || n.element.image >= runs.size()) return;
    const ::string::image_run& r = runs[n.element.image];
    std::uint32_t slot = 0;
    switch (r.source)
    {
        case ::string::image_source::glyph_atlas: slot = atlas_slot; break;
        default: return;   // unknown source: draw nothing rather than sampling an arbitrary slot
    }
    const std::array<float, 4> tint = to_linear(n.element.color.a == 0
                                                    ? ::string::color{ 255, 255, 255, 255 }
                                                    : n.element.color);
    out.push_back(GpuImage{
        { static_cast<float>(box.x), static_cast<float>(box.y),
          static_cast<float>(box.dimension.width), static_cast<float>(box.dimension.height) },
        { r.range_min, r.range_max, static_cast<float>(r.mip),
          r.false_colour ? 1.0f : 0.0f },
        { slot, r.channels, 0u, 0u },
        { tint[0], tint[1], tint[2], tint[3] } });
}

// Turn a text node into positioned glyph quads.
//
// The SHAPING — UTF-8 decode, kerning, whole-pixel advances, '\n', word wrap — is not done here. It
// is `string::shape_text`, the same call `dynamic_text_measurer` makes to decide how big this node's
// box should be. This pass supplies only what measurement has no opinion about: where the box landed,
// what colour the glyphs are, and which of them are outside the box and can be dropped.
//
// It used to be a second implementation of that algorithm, carrying a comment saying it had to match
// the measurer exactly or text would draw more lines than its box reserved. The two are now the same
// code, so they cannot drift.
void append_glyphs(::string::dynamic_font_atlas& atlas, ::string::text_shape_cache& cache,
                   const ::string::layout_node& n, const ::string::bounding_box& box,
                   std::span<const ::string::text_run> texts, std::vector<GpuGlyph>& glyphs)
{
    if (n.element.text == 0 || n.element.text >= texts.size()) return;

    const std::array<float, 4> color = to_linear(n.element.color);
    const ::string::text_run& run = texts[n.element.text];
    const float px = run.font_px > 0 ? static_cast<float>(run.font_px) : atlas.bake_px();
    const float s = px / atlas.bake_px();

    // Glyphs come back in run-local pixels and UNSNAPPED; translating to screen space and rounding
    // there — in that order — is this caller's job. Snapping in the shaper instead is not equivalent,
    // because std::round is not translation-invariant across zero (see text_shaper.hpp).
    const float origin_x = static_cast<float>(box.x);
    const float origin_y = static_cast<float>(box.y);
    const float clip_x1 = origin_x + box.dimension.width;
    const float clip_y1 = origin_y + box.dimension.height;

    // Wrap contract shared with dynamic_text_measurer: word-wrap elements that opted in with
    // `.wrap()`, plus fixed-width elements (whose width was always predictable at measure time, so
    // they have wrapped since before the opt-in existed). Everything else is one unwrapped line,
    // clipped. Both cases wrap to the FINAL BOX WIDTH.
    const bool do_wrap = n.element.wrap ||
                         n.element.sizing.width.mode == ::string::size_mode::FIXED;
    const float wrap = do_wrap ? static_cast<float>(box.dimension.width) : 0.0f;

    // REPLAYED FROM THE CACHE, not re-shaped. The measurer already shaped this exact run to size the
    // node, so the walk is done; what remains is translating, snapping and clipping — the parts that
    // depend on where the node landed and therefore could never have been cached with it.
    //
    // This is why the cache stores UNCLIPPED placements: clipping before caching would poison the
    // entry, and the same string replayed at a different scroll offset would come back missing the
    // glyphs that happened to be outside the box the first time it was seen.
    const ::string::text_shape_cache::shaped_run& shaped = cache.get(atlas, run.str, s, wrap);
    for (const ::string::placed_glyph& g : shaped.glyphs)
    {
        // Whole-pixel quad placement: consistent stems and letter spacing at minified SDF sizes.
        const float gx = std::round(origin_x + g.x);
        const float gy = std::round(origin_y + g.y);
        // Drop glyphs fully outside the node box (scroll/tooltip containment).
        if (gx + g.w > origin_x && gx < clip_x1 + 1.0f && gy + g.h > origin_y && gy < clip_y1 + 1.0f)
        {
            glyphs.push_back(GpuGlyph{
                { gx, gy, g.w, g.h },
                { g.metrics->u0, g.metrics->v0, g.metrics->u1, g.metrics->v1 },
                { color[0], color[1], color[2], color[3] },
            });
        }
    }
}

}  // namespace

void UIPass::make_ring(std::vector<Ring>& ring, std::uint32_t frames_in_flight,
                       std::uint32_t capacity, std::size_t stride)
{
    ring.resize(frames_in_flight);
    for (Ring& r : ring)
    {
        r.buffer = allocator_.create_resource(::string::gpu::buffer_info{
            .size = capacity * stride,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
            .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                              | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        });
        descriptor_table_.bind(r.buffer, ::string::gpu::descriptor_type::STORAGE_BUFFER);
        r.slot = descriptor_table_.get_binding_slot(r.buffer, ::string::gpu::descriptor_type::STORAGE_BUFFER);
        r.mapped = allocator_.get_buffer(r.buffer).allocation_info.pMappedData;
    }
}

UIPass::UIPass(engine_context& context, std::shared_ptr<::string::dynamic_font_atlas> atlas,
               Author author, PostLayout post_layout, DeferredAuthor deferred)
: device_(context.device)
, allocator_(context.allocator)
, descriptor_table_(context.descriptor_table)
, atlas_(std::move(atlas))
, author_(std::move(author))
, post_layout_(std::move(post_layout))
, deferred_(std::move(deferred))
, input_(context.input)
, input_map_(context.input_map)
, shader_registry_(context.shader_registry)
{
    // The UI's OWN bindings. Defaults only — an app or a player profile may rebind them, which is
    // the entire point of routing UI input through the same map gameplay uses. Bound by NAME so the
    // rebinding UI can show them; read by interned id in populate_interaction.
    input_map_.bind_button("ui.activate", String::GamepadButton::A);
    input_map_.bind_button("ui.submit", String::KeyCode::ENTER);
    input_map_.bind_button("ui.cancel", String::KeyCode::ESCAPE);
    // The UI reads its actions as its OWN context, pushed above base. Two consequences, both wanted:
    // gameplay keeps receiving anything the UI does not claim, and a text surface raising
    // text_capture (which suppresses BASE) does not stop the UI acting on its own Escape.
    //
    // CLAIMING NOTHING, and the empty list is load-bearing: the one-argument push is the EXCLUSIVE
    // overload, which claims everything below it. Using it here put a blanket claim over base and
    // silently killed all gameplay input — camera and every debug key — while leaving the UI itself
    // working, so nothing looked broken from the UI side. The context exists to give the UI a
    // POSITION on the stack to read from, not to take anything; what gets claimed is a per-surface
    // decision.
    input_map_.push_context(kUiContext, std::span<const ::String::ActionId>{});

    const std::filesystem::path& resources_path = context.resources_path;

    // --- Dynamic SDF atlas: an R8 sampled image, seeded here and re-uploaded (dirty region) each
    // frame the atlas grows (record_compute). ---
    atlas_upload_bytes_ = static_cast<std::uint32_t>(atlas_->pixels_size());
    atlas_image_ = allocator_.create_resource(::string::gpu::image_info{
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
    for (::string::gpu::resource_id& s : atlas_staging_)
    {
        s = allocator_.create_resource(::string::gpu::buffer_info{
            .size = atlas_upload_bytes_,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
            .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                              | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        });
    }
    descriptor_table_.bind(atlas_image_, ::string::gpu::descriptor_type::TEXTURE);
    atlas_slot_ = descriptor_table_.get_binding_slot(atlas_image_, ::string::gpu::descriptor_type::TEXTURE);
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
    make_ring(image_ring_, context.frames_in_flight, kMaxImages, sizeof(GpuImage));
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
    auto overlay_builder = [global_layout, samples](::string::gpu::device& dev,
                                                    const ::string::gpu::compiled_program& compiled) {
        ::string::gpu::pipeline p{};
        p.push_constants = compiled.layout.push_constant;
        p.pipeline_layout = ::string::gpu::pipeline_layout_builder()
            .set_descriptor_set_layout({ global_layout })
            .set_push_constant_ranges({ compiled.layout.push_constant })
            .build(dev);

        ::string::gpu::pipeline_builder builder(dev);
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
        p.pipeline_type = ::string::gpu::pipeline_type::GRAPHICS;
        return p;
    };

    shape_program_ = context.shader_registry.create(resources_path / "shaders" / "ui_shader.slang",
                                                    overlay_builder);
    text_program_ = context.shader_registry.create(resources_path / "shaders" / "text_shader.slang",
                                                   overlay_builder);
    image_program_ = context.shader_registry.create(resources_path / "shaders" / "image_shader.slang",
                                                    overlay_builder);
}

UIPass::~UIPass()
{
    const ::string::gpu::pipeline& shape_p = shape_program_->current();
    vkDestroyPipeline(device_.get_device(), shape_p.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), shape_p.pipeline_layout, nullptr);
    const ::string::gpu::pipeline& text_p = text_program_->current();
    vkDestroyPipeline(device_.get_device(), text_p.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), text_p.pipeline_layout, nullptr);
    const ::string::gpu::pipeline& image_p = image_program_->current();
    vkDestroyPipeline(device_.get_device(), image_p.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), image_p.pipeline_layout, nullptr);
    for (Ring& r : shape_ring_)
    {
        descriptor_table_.unbind(r.buffer, ::string::gpu::descriptor_type::STORAGE_BUFFER);
        allocator_.destroy_resource(r.buffer);
    }
    for (Ring& r : glyph_ring_)
    {
        descriptor_table_.unbind(r.buffer, ::string::gpu::descriptor_type::STORAGE_BUFFER);
        allocator_.destroy_resource(r.buffer);
    }
    for (Ring& r : image_ring_)
    {
        descriptor_table_.unbind(r.buffer, ::string::gpu::descriptor_type::STORAGE_BUFFER);
        allocator_.destroy_resource(r.buffer);
    }
    descriptor_table_.unbind(atlas_image_, ::string::gpu::descriptor_type::TEXTURE);
    allocator_.destroy_resource(atlas_image_);
    for (::string::gpu::resource_id s : atlas_staging_)
    {
        allocator_.destroy_resource(s);
    }
    vkDestroySampler(device_.get_device(), atlas_sampler_, nullptr);
}

void UIPass::author_error_overlay()
{
    const std::vector<::string::gpu::compile_error> errors = shader_registry_.current_errors();
    if (errors.empty())
    {
        error_lines_.clear();
        return;  // last compile succeeded — overlay clears
    }

    // Flatten the diagnostics into individual lines (Slang messages already carry file:line:col).
    error_lines_.clear();
    for (const ::string::gpu::compile_error& e : errors)
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
// de-edged intent and nothing else, which is what keeps `::string::ui` platform-free and testable.
::string::ui::interaction_input UIPass::populate_interaction(float delta_time)
{
    ::string::ui::interaction_input in;
    const glm::vec2 mouse = input_.mouse_position();
    in.cursor_x = mouse.x;
    in.cursor_y = mouse.y;
    in.ui_mode = !input_.mouse_captured();
    in.primary_down = input_.mouse_button_down(MouseButton::LEFT);
    in.primary_pressed = input_.mouse_button_pressed(MouseButton::LEFT);
    in.primary_released = prev_click_ && !in.primary_down;
    prev_click_ = in.primary_down;
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

    // --- Text entry. Composed characters come from the platform's per-frame buffer (layout, shift
    // and IME already applied); backspace and enter are key EDGES, which `key_pressed` already
    // de-edges. The view is consumed the same frame, so it never outlives the buffer.
    in.typed_text = input_.typed_text();

    // Editing keys use key_edit (press OR auto-repeat): holding Left must walk the caret, and the
    // repeat delay/rate are the platform's, not ours to invent. Modifier CONVENTIONS are resolved
    // here — which chord means copy is a platform matter (Cmd on macOS), so the kit never learns
    // what Ctrl is.
    const bool ctrl = input_.key_down(String::KeyCode::LEFT_CONTROL) ||
                      input_.key_down(String::KeyCode::RIGHT_CONTROL);
    const bool shift = input_.key_down(String::KeyCode::LEFT_SHIFT) ||
                       input_.key_down(String::KeyCode::RIGHT_SHIFT);
    in.backspace = input_.key_edit(String::KeyCode::BACKSPACE);
    in.del = input_.key_edit(String::KeyCode::DELETE);
    in.caret_left = input_.key_edit(String::KeyCode::LEFT);
    in.caret_right = input_.key_edit(String::KeyCode::RIGHT);
    in.caret_home = input_.key_edit(String::KeyCode::HOME);
    in.caret_end = input_.key_edit(String::KeyCode::END);
    in.select_mod = shift;
    in.word_mod = ctrl;
    in.copy = ctrl && input_.key_pressed(String::KeyCode::C);
    in.cut = ctrl && input_.key_pressed(String::KeyCode::X);
    in.paste = ctrl && input_.key_pressed(String::KeyCode::V);
    in.select_all = ctrl && input_.key_pressed(String::KeyCode::A);
    in.clipboard = input_.clipboard_text();

    // --- UI ACTIONS, resolved through the SAME remappable map gameplay uses. The kit never learns
    // what a key is; it receives intent bits. Read at the base context, so the UI deliberately does
    // NOT claim these away from gameplay just by existing — a surface that wants exclusivity pushes
    // its own context (which is what the console does via text_capture).
    using ::string::ui_action;
    static constexpr ::String::ActionId kActivate = ::String::action_id("ui.activate");
    static constexpr ::String::ActionId kSubmit   = ::String::action_id("ui.submit");
    static constexpr ::String::ActionId kCancel   = ::String::action_id("ui.cancel");
    const auto offer = [&](ui_action a, ::String::ActionId bound) {
        if (input_map_.pressed(bound, kUiContext)) in.actions |= ::string::action_bit(a);
    };
    offer(ui_action::activate, kActivate);
    offer(ui_action::submit,   kSubmit);
    offer(ui_action::cancel,   kCancel);
    in.scroll_y = input_.scroll_y();

    return in;
}

// The engine REQUESTS a mode; the host grants it. Kept out of the resolvers deliberately: capture is
// a platform decision, and `::string::ui` must not know that a window or a cursor exists.
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

    const std::string text = ::string::dump_layout(builder_, label);
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
    // live in the engine (::string::ui); this pass only supplies the raw signals.
    const ::string::ui::interaction_input in = populate_interaction(delta_time);
    ::string::ui::begin_interaction(interaction_, in);
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
    builder_.begin(::string::format{ .padding = { 12, 12, 12, 12 }, .gap = 8,
                                   .direction = ::string::direction::VERTICAL });
    author_(builder_, UiContext{ input_, input_map_, interaction_ });
    author_error_overlay();
    const ::string::dimension available{
        static_cast<std::uint16_t>(std::min<std::uint32_t>(screen_size.width, 0xFFFF)),
        static_cast<std::uint16_t>(std::min<std::uint32_t>(screen_size.height, 0xFFFF)) };
    builder_.end(available, ::string::dynamic_text_measurer{ atlas_.get(), builder_.text_runs(), 0,
                                                             &shape_cache_ });

    // DEFERRED SURFACES (brief 12b M1): popups anchored to geometry that only exists now. Each is
    // its own outermost tree, so it lays out independently against the screen — and because the main
    // tree has already resolved, a popup can read its anchor's box from THIS frame instead of last
    // frame's. A body may declare another, so this drains rather than iterating a snapshot.
    while (deferred_ && deferred_())
    {
        builder_.end(available, ::string::dynamic_text_measurer{ atlas_.get(),
                                                                 builder_.text_runs(), 0,
                                                                 &shape_cache_ });
    }

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
    const std::vector<clip_rect> clips = clip_rects(builder_, available);
    std::vector<std::uint16_t> layers(keys.begin(), keys.end());
    std::sort(layers.begin(), layers.end());
    layers.erase(std::unique(layers.begin(), layers.end()), layers.end());

    std::vector<GpuShape> shapes;
    std::vector<GpuGlyph> glyphs;
    std::vector<GpuImage> images;
    std::vector<DrawBatch>& batches = batches_[current_frame];
    batches.clear();

    // Within a layer, nodes are walked in TREE ORDER and a batch ends wherever the clip region
    // changes — RUN-LENGTH, not group-by. Grouping every node sharing a clip would reorder draws
    // within the layer and break painter order, which is the one thing the layer batching exists to
    // preserve.
    const auto flush = [&](std::size_t s0, std::size_t g0, std::size_t i0, const clip_rect& c) {
        if (shapes.size() == s0 && glyphs.size() == g0 && images.size() == i0) return;
        batches.push_back(DrawBatch{ static_cast<std::uint32_t>(s0),
                                     static_cast<std::uint32_t>(shapes.size() - s0),
                                     static_cast<std::uint32_t>(g0),
                                     static_cast<std::uint32_t>(glyphs.size() - g0),
                                     static_cast<std::uint32_t>(i0),
                                     static_cast<std::uint32_t>(images.size() - i0),
                                     c.x0, c.y0, c.x1 - c.x0, c.y1 - c.y0 });
    };

    for (const std::uint16_t layer : layers)
    {
        bool have = false;
        clip_rect current{};
        std::size_t s0 = shapes.size();
        std::size_t g0 = glyphs.size();
        std::size_t i0 = images.size();
        for (std::size_t ni = 0; ni < builder_.nodes().size(); ++ni)
        {
            if (keys[ni] != layer) continue;
            const clip_rect& c = clips[ni];
            if (c.empty()) continue;   // fully scrolled out: nothing to draw at all
            if (!have) { current = c; have = true; }
            else if (!(c == current))
            {
                flush(s0, g0, i0, current);
                s0 = shapes.size();
                g0 = glyphs.size();
                i0 = images.size();
                current = c;
            }
            const ::string::layout_node& n = builder_.nodes()[ni];
            // Resolved once here, not three times below: this is the ONE point where a node's
            // surface-local box becomes the framebuffer rect every stream draws into.
            const ::string::bounding_box box = builder_.screen_box(n);
            // Image first: an image node carries neither text nor a fill, so every EXISTING node still
            // takes exactly the branch it took before — this stream is additive, not a reordering.
            GpuShape sh{};
            if (n.element.image != 0) append_image(n, box, builder_.image_runs(), atlas_slot_, images);
            else if (shape_of(n, box, sh)) shapes.push_back(sh);
            else append_glyphs(*atlas_, shape_cache_, n, box, builder_.text_runs(), glyphs);
        }
        if (have) flush(s0, g0, i0, current);
    }
    if ((shapes.size() > kMaxShapes || glyphs.size() > kMaxGlyphs || images.size() > kMaxImages)
        && !warned_overflow_)
    {
        STRING_LOG_WARN("UIPass: content exceeds ring capacity ({} shapes / {} glyphs / {} images);"
                        " truncating", shapes.size(), glyphs.size(), images.size());
        warned_overflow_ = true;
    }
    Ring& sr = shape_ring_[current_frame];
    sr.count = static_cast<std::uint32_t>(std::min<std::size_t>(shapes.size(), kMaxShapes));
    std::memcpy(sr.mapped, shapes.data(), sr.count * sizeof(GpuShape));
    Ring& gr = glyph_ring_[current_frame];
    gr.count = static_cast<std::uint32_t>(std::min<std::size_t>(glyphs.size(), kMaxGlyphs));
    std::memcpy(gr.mapped, glyphs.data(), gr.count * sizeof(GpuGlyph));
    Ring& ir = image_ring_[current_frame];
    ir.count = static_cast<std::uint32_t>(std::min<std::size_t>(images.size(), kMaxImages));
    if (ir.count) std::memcpy(ir.mapped, images.data(), ir.count * sizeof(GpuImage));

    // Clamp the batch ranges to what actually fit. Without this an overflowing frame would draw
    // from indices past the ring's contents — truncation must drop whole tail batches, not read
    // stale memory.
    for (DrawBatch& b : batches)
    {
        b.shape_count = b.shape_first >= sr.count
                            ? 0u : std::min(b.shape_count, sr.count - b.shape_first);
        b.glyph_count = b.glyph_first >= gr.count
                            ? 0u : std::min(b.glyph_count, gr.count - b.glyph_first);
        b.image_count = b.image_first >= ir.count
                            ? 0u : std::min(b.image_count, ir.count - b.image_first);
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
    // and gamepad focus-nav are ALL engine rules now (::string::ui) — deleted from this pass rather
    // than wrapped. The pass keeps only what is genuinely the host's: producing the raw signals
    // above, and applying the mode requests below (capture is a platform decision).
    ::string::ui::resolve_interaction(interaction_, in, builder_);
    apply_mode_requests();

    // Age the shaping cache LAST: both the measurer (during layout) and the glyph packing above mark
    // their entries seen, so anything untouched by either is genuinely gone from the UI this frame.
    // Doing it earlier would evict runs the pass had not replayed yet.
    shape_cache_.end_frame();
}

bool UIPass::record_compute(::string::gpu::command_recorder& recorder, uint16_t current_frame)
{
    // Re-upload the dynamic atlas when new glyphs appeared this frame (the author's measure/shape
    // rasterised them into the CPU atlas during update()). Runs OUTSIDE dynamic rendering, so the
    // image-layout barriers are legal here. Whole-atlas copy (simple + the atlas is ~1MB); the
    // dirty flag makes it a no-op on the common no-new-glyph frame.
    if (current_frame >= atlas_staging_.size() || !atlas_->take_dirty())
    {
        return false;
    }

    const ::string::gpu::allocated_buffer& staging = allocator_.get_buffer(atlas_staging_[current_frame]);
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

void UIPass::record(::string::gpu::command_recorder& recorder, uint16_t current_frame)
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
    const ::string::gpu::pipeline& shape_p = shape_program_->current();
    const ::string::gpu::pipeline& text_p = text_program_->current();
    const ::string::gpu::pipeline& image_p = image_program_->current();
    const Ring& sr = shape_ring_[current_frame];
    const Ring& gr = glyph_ring_[current_frame];
    const Ring& ir = image_ring_[current_frame];

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

    const auto draw_images = [&](std::uint32_t first, std::uint32_t count) {
        if (count == 0) return;
        recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, image_p.pipeline);
        recorder.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_GRAPHICS,
            image_p.pipeline_layout, 0, 1, &descriptor_set_, 0, nullptr);
        const ImagePush push{
            { static_cast<float>(screen_size.width), static_cast<float>(screen_size.height) },
            ir.slot, first, inv_exposure };
        recorder.push_constants(image_p.pipeline_layout, image_p.push_constants.stageFlags,
            0, sizeof(ImagePush), &push);
        recorder.draw(6, count, 0, 0);
    };

    // One shapes-then-text pair per layer, in ascending key order. This is what makes a raised
    // panel cover a lower panel's text as well as its background.
    for (const DrawBatch& b : batches_[current_frame])
    {
        // Scissor is dynamic state on both pipelines, so a clip region costs one extra command per
        // batch and no pipeline churn. Clamped to the framebuffer: Vulkan rejects a negative offset,
        // and a clip region CAN legitimately start off-screen now that layout positions are signed.
        const int cx = std::max(0, b.clip_x);
        const int cy = std::max(0, b.clip_y);
        const int cw = std::min<int>(b.clip_w - (cx - b.clip_x), static_cast<int>(screen_size.width) - cx);
        const int ch = std::min<int>(b.clip_h - (cy - b.clip_y), static_cast<int>(screen_size.height) - cy);
        if (cw <= 0 || ch <= 0) continue;
        recorder.set_scissor(VkRect2D{ { cx, cy },
                                      { static_cast<std::uint32_t>(cw), static_cast<std::uint32_t>(ch) } });
        // shapes -> images -> text within a batch: an image sits ON its panel background and UNDER
        // any label drawn over it, which is the only order that lets a widget caption read.
        draw_shapes(b.shape_first, b.shape_count);
        draw_images(b.image_first, b.image_count);
        draw_text(b.glyph_first, b.glyph_count);
    }
}

}  // namespace string::render
