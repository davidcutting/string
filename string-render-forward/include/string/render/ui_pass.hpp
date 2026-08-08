#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <string/ui/dynamic_font.hpp>
#include <string/ui/font.hpp>
#include <string/ui/layout.hpp>
#include <string/ui/text_shaper.hpp>
#include <string/ui/interaction.hpp>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <string/gpu/resource.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/platform/input.hpp>
#include <string/platform/input_map.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/frame_graph.hpp>
#include <string/gpu/pass_context.hpp>

#include <volk.h>

namespace string::render
{

// The scissor region a node draws within: the intersection of every `clip` ancestor's box, in SCREEN
// space. An empty region means the node is entirely scrolled/panned out and draws nothing.
//
// Here rather than in the .cpp only so the pass can hold a persistent buffer of them (brief 12c
// M2a); it is the packer's type and nothing outside the packer should need it.
struct clip_rect
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool operator==(const clip_rect&) const = default;
    [[nodiscard]] bool empty() const { return x1 <= x0 || y1 <= y0; }
};

// The three GPU instance streams the packer emits, each matching a `struct` in its shader:
// `Shape` in ui_shader, `Glyph` in text_shader, `Image` in image_shader. Here rather than in the
// .cpp because the pass RETAINS them per surface across frames (brief 12c M2b).
//
// All three carry ABSOLUTE screen coordinates in `rect.xy`, which is what makes retention work: a
// surface that only MOVED is its retained stream with a placement delta added to those two floats,
// never a re-emission. Placement is integral, so a glyph's whole-pixel rounding survives the shift
// exactly — round(o + d + g) == round(o + g) + d for integral d, and that identity is the reason
// the translate is a legal substitute for re-packing rather than an approximation of it.
struct GpuShape
{
    float rect[4];    // pos.xy, size.xy (px)
    float fill[4];    // linear rgba
    float stroke[4];  // linear rgba
    float params[4];  // corner_radius, stroke_width, sweep, _
};
struct GpuGlyph
{
    float rect[4];   // x, y, w, h (px)
    float uv[4];     // u0, v0, u1, v1
    float color[4];  // linear rgba
};
struct GpuImage
{
    float rect[4];              // pos.xy, size.xy (px)
    float range[4];             // range_min, range_max, mip, false_colour
    std::uint32_t params[4];    // texture slot, channel mask, _, _
    float tint[4];              // linear rgba multiplier
};

// The one UI overlay pass: rounded-rect shapes AND SDF text, both from a SINGLE per-frame layout
// tree. The application supplies an authoring callback run every frame; the pass lays it out (with
// a font measurer), packs the rect/container nodes into a shape buffer and the text nodes into a
// glyph buffer, and draws shapes (under) then glyphs (over) — so text can sit inside a laid-out
// colored container. Dynamic: labels, hover highlights and typed fields update per frame via
// persistent-mapped per-frame ring buffers.
//
// Interaction: the pass hit-tests the cursor against its layout each frame and tracks a focused
// element (set by a click in UI mode). It hands the author the live input + hovered/focused ids so
// it can style and route text. Game vs UI mode is the cursor-capture state (Input): captured =
// mouse-look/WASD to the camera, free = clicks/typing to the UI. A click on empty UI space requests
// game mode again (the pass owns that decision, so the UI gets first dibs on a click).
//
// Brief 20 — a plain application-owned object, not a Pass subclass. It authors ITSELF onto the app's
// frame_graph in declare() and records through pass_context; tick() is ordinary per-frame CPU work
// the app calls before the graph executes.
class ui_pass
{
public:
    // State handed to the authoring callback each frame so it can style/route by interaction.
    struct UiContext
    {
        // Non-const so a modal surface (the debug console) can set Input::text_capture to suppress
        // gameplay input while it is open. Read-only for every other author.
        string::Input& input;
        // The remappable action map. Non-const so a surface can push/pop its own input CONTEXT (a
        // modal claiming everything, a rebinder claiming keys while it captures one) and so a
        // bindings UI can actually rebind. Everything else should read actions, not raw keys.
        string::InputMap& input_map;
        // Resolved interaction (brief 12 M0b): hovered/focused/pressed ids, drag state, dt. Owned by
        // the engine (`::string::ui`) — the pass PRODUCES it and hands it over, it does not define it.
        // Replaces the loose fields this struct used to duplicate.
        const ::string::ui::interaction& ui;
    };
    // Authors the frame's UI into a builder already opened at a screen-filling root container.
    using Author = std::function<void(::string::layout_builder&, const UiContext&)>;
    // Runs AFTER layout resolves, before packing. The one post-layout seam: a workspace needs its
    // resolved region sizes to turn a splitter drag into a sizing change, and those do not exist at
    // authoring time. Deliberately narrow — it reads sizes, it does not author.
    using PostLayout = std::function<void(const ::string::layout_builder&)>;
    // Emits ONE deferred surface (brief 12b M1) and returns false when none are left. The pass drives
    // the loop because only it holds the measurer that closes each surface's layout.
    using DeferredAuthor = std::function<bool()>;

    // `atlas` is the dynamic (grow-on-demand, Unicode) SDF glyph atlas — shared, mutated as new
    // glyphs are seen. `author` declares the whole UI (shapes + text) each frame. `samples` is the
    // MSAA sample count of the scene target this overlay draws into: pipeline fixed state, supplied
    // by the app exactly like composite_pass's colour format (engine_context no longer carries it).
    ui_pass(string::engine_context& context, VkSampleCountFlagBits samples,
            std::shared_ptr<::string::dynamic_font_atlas> atlas,
            Author author, PostLayout post_layout = {}, DeferredAuthor deferred = {});
    ~ui_pass();

    ui_pass(const ui_pass&) = delete;
    ui_pass& operator=(const ui_pass&) = delete;

    // Author this pass onto the graph. Two declarations:
    //
    //   ui.atlas_upload   a TRANSFER pass, conditional on the glyph atlas having grown this frame,
    //                     that copies the CPU atlas into the atlas image
    //   ui                the overlay draw, which SAMPLES that atlas and writes `target`
    //
    // The write->read edge between them is derived from those two declarations, as is the
    // cross-frame WAR against the previous frame's text draw. There are no hand-rolled barriers here.
    void declare(::string::frame_graph& fg, ::string::gpu::image target);

    // Per-frame CPU work: interaction, authoring, layout, packing into this frame's rings. Ordinary
    // app code, called before the graph executes. `screen` and `frame_slot` are what the deleted
    // Pass::resize()/update(dt, current_frame) lifecycle used to supply; `frame_slot` MUST be the
    // same slot the graph then executes with, because it selects the ring buffer record() draws from.
    void tick(float delta_time, VkExtent2D screen, std::uint32_t frame_slot);

    // Has the glyph atlas grown since the last upload? Non-consuming: the graph's conditional asks
    // this every frame, and the upload callback is what clears it.
    bool atlas_dirty() const { return atlas_dirty_; }

private:
    // The recording callbacks, bound in declare().
    void record(::string::pass_context& ctx);
    // Copies the whole CPU atlas into this frame slot's staging buffer and into the atlas image.
    void record_atlas_upload(::string::pass_context& ctx);

    // Per-frame ring capacities. Raised for the brief-05 500-nameplate synthetic stress (each
    // nameplate is a name run + 1-2 bars; 500 of them plus screens fit comfortably here). Content
    // beyond these is dropped (with a one-time warn).
    static constexpr std::uint32_t kMaxShapes = 16384;
    static constexpr std::uint32_t kMaxGlyphs = 65536;
    // Images are one instance per WIDGET, not per glyph or per node — a browser panel shows a
    // handful at most, so this is deliberately small.
    static constexpr std::uint32_t kMaxImages = 256;

    // A persistent-mapped storage buffer per frame in flight (the geometry_streamer pattern).
    struct Ring
    {
        ::string::gpu::resource_id buffer = 0;
        std::uint32_t slot = 0;
        void* mapped = nullptr;
        std::uint32_t count = 0;
    };
    // One draw pair per layer key (see layer_keys in the .cpp). Ranges into the shape and glyph
    // rings; record() draws a batch's shapes then its text before moving to the next, so a raised
    // panel covers a lower one's TEXT and not just its background.
    struct DrawBatch
    {
        std::uint32_t shape_first = 0;
        std::uint32_t shape_count = 0;
        std::uint32_t glyph_first = 0;
        std::uint32_t glyph_count = 0;
        std::uint32_t image_first = 0;
        std::uint32_t image_count = 0;
        // Scissor region for this batch, in pixels. Set before the batch draws.
        int clip_x = 0;
        int clip_y = 0;
        int clip_w = 0;
        int clip_h = 0;
    };
    // Create `frames_in_flight` mapped storage buffers of `capacity * stride` bytes, bound bindless.
    void make_ring(std::vector<Ring>& ring, std::uint32_t frames_in_flight, std::uint32_t capacity,
                   std::size_t stride);

    ::string::gpu::device& device_;
    ::string::gpu::resource_allocator& allocator_;
    ::string::gpu::descriptor_table& descriptor_table_;

    std::shared_ptr<::string::dynamic_font_atlas> atlas_;
    Author author_;
    PostLayout post_layout_;
    DeferredAuthor deferred_;
    string::Input& input_;  // non-const: the pass requests game/UI capture mode
    // The UI's own actions are bound here like any other, so which key means "cancel" is a player
    // setting rather than a hardcoded key_pressed() in the crossing below.
    string::InputMap& input_map_;
    // Read each frame for the shader-compile error overlay (brief 01, M4): when a hot-reload fails,
    // the registry holds the diagnostics; the pass draws them over the UI until the next success.
    ::string::gpu::shader_program_registry& shader_registry_;
    ::string::layout_builder builder_;
    // Brief 12b M0. Shared by the MEASURER (sizing a node) and this pass (placing its quads), which
    // is what turns two shapes per string per frame into one computation and two lookups. Aged once
    // per frame, like Motion.
    ::string::text_shape_cache shape_cache_;
    // Brief 12 M0b: the pass no longer OWNS interaction state — it owns the engine's value type and
    // the single crossing that fills it. Everything the UI reads lives in `interaction_`.
    ::string::ui::interaction interaction_{};
    bool prev_click_ = false;  // for left-button edge detection in UI mode
    bool stick_armed_ = true;  // gamepad left-stick recentred since last focus-nav flick

    // THE ONE CROSSING (brief 12 M0b, load-bearing): the only place sandbox translates platform
    // input into engine UI vocabulary. Option (3) — engine owns a `ui::host` — is then "move this
    // function", not a redesign. Do not spread `string::Input` reads into other UI code.
    ::string::ui::interaction_input populate_interaction(float delta_time);
    // Applies the engine's mode REQUESTS (click on empty space -> game, focus-nav -> UI) back to
    // the platform. Separate from the resolvers because capture is the host's decision, not theirs.
    void apply_mode_requests();
    // Feel instrumentation (brief 05 M5): timestamp a UI click in update() and, in record() of the
    // SAME frame (the immediate-mode author reflects the click that frame), log the click->record
    // latency. Reports the numbers the brief asks for; gated so it's quiet unless a click happened.
    std::chrono::steady_clock::time_point click_time_{};
    bool click_pending_ = false;
    // Rolling UI CPU-cost instrumentation (author + layout + pack), logged every 300 frames.
    std::int64_t ui_cpu_us_accum_ = 0;
    std::int64_t ui_cpu_us_peak_ = 0;
    int ui_cpu_samples_ = 0;
    // Per-PHASE breakdown of the same total (brief 12c M0). The combined number above says the UI
    // costs N ms; it cannot say which of the four things to attack, and 12b's milestones removed
    // enough that the answer changed. Authoring and packing are the two that still run
    // unconditionally every frame, so they are what brief 12c is scoped against.
    std::int64_t ui_author_us_accum_ = 0;
    std::int64_t ui_layout_us_accum_ = 0;
    std::int64_t ui_pack_us_accum_ = 0;
    // Prep (batch keys + clip regions) split out of pack: it is the part retention still has to pay
    // even when a surface is clean, so it needs its own number (brief 12c M2a).
    std::int64_t ui_prep_us_accum_ = 0;
    std::int64_t ui_upload_us_accum_ = 0;

    // Packing scratch, PERSISTENT so a steady-state frame allocates nothing (brief 12c M2a). These
    // are two node-sized arrays and a two-entry layer set that were rebuilt from scratch every
    // frame; at nameplate scale that was a measurable share of packing on its own.
    std::vector<std::uint16_t> pack_keys_;
    std::vector<clip_rect> pack_clips_;
    std::vector<std::uint16_t> pack_layers_;

    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;

    // Shape overlay (rounded-rect SDF): ui_shader.slang. Pipeline owned by the hot-reloadable
    // shader_program (recompiles + swaps on save); the pass binds shape_program_->current().
    ::string::gpu::shader_program* shape_program_ = nullptr;
    std::vector<Ring> shape_ring_;

    // Text overlay (glyph SDF sampling the atlas): text_shader.slang.
    ::string::gpu::shader_program* text_program_ = nullptr;
    std::vector<Ring> glyph_ring_;

    // Image widget (brief 14 M4): image_shader.slang. A THIRD stream beside shapes and glyphs — the
    // per-layer batching already accommodated it, so this adds a ring and a draw, not a mechanism.
    ::string::gpu::shader_program* image_program_ = nullptr;
    std::vector<Ring> image_ring_;
    std::vector<std::vector<DrawBatch>> batches_;   // per frame-in-flight, parallel to the rings
    ::string::gpu::resource_id atlas_image_ = 0;
    // The same atlas as a LOGICAL handle, registered with the graph in declare(): the upload pass
    // writes it, the draw pass samples it, and that pair is what the graph derives the sync from.
    ::string::gpu::image atlas_handle_{};
    // The atlas's bindless texture slot, resolved once at construction. Needed at PACK time (an
    // image widget sourced from the glyph atlas is packed into the ring during tick(), long before
    // any pass_context exists); the draw's own atlas slot comes from ctx.slot() at record time.
    std::uint32_t atlas_slot_ = 0;
    // Dynamic-atlas re-upload: a host-visible staging buffer per frame in flight (the atlas grows as
    // new glyphs appear). The declared ui.atlas_upload transfer pass copies the whole CPU atlas into
    // this frame's staging and vkCmdCopyBufferToImage's it whenever the atlas reported dirty.
    std::vector<::string::gpu::resource_id> atlas_staging_;
    std::uint32_t atlas_upload_bytes_ = 0;
    // Latched in tick() from the atlas's CONSUMING take_dirty(), so the graph's conditional can ask
    // "is it dirty?" every frame without eating the flag. Cleared by the upload callback.
    bool atlas_dirty_ = false;
    // This frame's screen size, latched in tick(). record() uses pass_context::extent instead.
    VkExtent2D screen_size{};

    bool warned_overflow_ = false;

    // Brief 12 M0a — layout-tree dump gate (dbg.ui.dump / dbg.ui.dump_frame). `ui_frames_` counts
    // AUTHORED frames, not frames-in-flight: the dump must name a specific authored tree, and
    // `current_frame` is a ring slot that repeats.
    std::uint64_t ui_frames_ = 0;
    bool dump_written_ = false;
    void maybe_dump_layout();

    // Persistent backing for the compile-error overlay's text (add_text takes non-owning views, so
    // the strings must outlive layout + record). Rebuilt each frame from the registry's diagnostics.
    std::vector<std::string> error_lines_;
    // Appends the shader-compile error overlay to the current layout tree, if any errors are live.
    void author_error_overlay();
};

}  // namespace string::render
