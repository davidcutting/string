#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <string/core/dynamic_font.hpp>
#include <string/core/font.hpp>
#include <string/core/layout.hpp>
#include <string/ui/interaction.hpp>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <string/gpu/resource.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/platform/input.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/render_pass.hpp>

#include <volk.h>

namespace sandbox
{

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
class UIPass final : public String::Pass
{
public:
    // State handed to the authoring callback each frame so it can style/route by interaction.
    struct UiContext
    {
        // Non-const so a modal surface (the debug console) can set Input::text_capture to suppress
        // gameplay input while it is open. Read-only for every other author.
        String::Input& input;
        // Resolved interaction (brief 12 M0b): hovered/focused/pressed ids, drag state, dt. Owned by
        // the engine (`string::ui`) — the pass PRODUCES it and hands it over, it does not define it.
        // Replaces the loose fields this struct used to duplicate.
        const string::ui::interaction& ui;
    };
    // Authors the frame's UI into a builder already opened at a screen-filling root container.
    using Author = std::function<void(string::layout_builder&, const UiContext&)>;
    // Runs AFTER layout resolves, before packing. The one post-layout seam: a workspace needs its
    // resolved region sizes to turn a splitter drag into a sizing change, and those do not exist at
    // authoring time. Deliberately narrow — it reads sizes, it does not author.
    using PostLayout = std::function<void(const string::layout_builder&)>;

    // `atlas` is the dynamic (grow-on-demand, Unicode) SDF glyph atlas — shared, mutated as new
    // glyphs are seen. `author` declares the whole UI (shapes + text) each frame.
    UIPass(String::engine_context& context, std::shared_ptr<string::dynamic_font_atlas> atlas,
           Author author, PostLayout post_layout = {});
    ~UIPass() override;

    UIPass(const UIPass&) = delete;
    UIPass& operator=(const UIPass&) = delete;

    // Stable identity for tooling (Tracy zones, inspector). Brief 06.
    std::string_view debug_name() const override { return "ui"; }

    void update(float delta_time, uint16_t current_frame) override;
    // Uploads the dynamic atlas's dirty region (glyphs added this frame) before the color pass draws.
    bool record_compute(string::gpu::command_recorder& recorder, uint16_t current_frame) override;
    void record(string::gpu::command_recorder& recorder, uint16_t current_frame) override;

private:
    // Per-frame ring capacities. Raised for the brief-05 500-nameplate synthetic stress (each
    // nameplate is a name run + 1-2 bars; 500 of them plus screens fit comfortably here). Content
    // beyond these is dropped (with a one-time warn).
    static constexpr std::uint32_t kMaxShapes = 16384;
    static constexpr std::uint32_t kMaxGlyphs = 65536;

    // A persistent-mapped storage buffer per frame in flight (the geometry_streamer pattern).
    struct Ring
    {
        string::gpu::resource_id buffer = 0;
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
        // Scissor region for this batch, in pixels. Set before the batch draws.
        int clip_x = 0;
        int clip_y = 0;
        int clip_w = 0;
        int clip_h = 0;
    };
    // Create `frames_in_flight` mapped storage buffers of `capacity * stride` bytes, bound bindless.
    void make_ring(std::vector<Ring>& ring, std::uint32_t frames_in_flight, std::uint32_t capacity,
                   std::size_t stride);

    string::gpu::device& device_;
    string::gpu::resource_allocator& allocator_;
    string::gpu::descriptor_table& descriptor_table_;

    std::shared_ptr<string::dynamic_font_atlas> atlas_;
    Author author_;
    PostLayout post_layout_;
    String::Input& input_;  // non-const: the pass requests game/UI capture mode
    // Read each frame for the shader-compile error overlay (brief 01, M4): when a hot-reload fails,
    // the registry holds the diagnostics; the pass draws them over the UI until the next success.
    string::gpu::shader_program_registry& shader_registry_;
    string::layout_builder builder_;
    // Brief 12 M0b: the pass no longer OWNS interaction state — it owns the engine's value type and
    // the single crossing that fills it. Everything the UI reads lives in `interaction_`.
    string::ui::interaction interaction_{};
    bool prev_click_ = false;  // for left-button edge detection in UI mode
    bool stick_armed_ = true;  // gamepad left-stick recentred since last focus-nav flick

    // THE ONE CROSSING (brief 12 M0b, load-bearing): the only place sandbox translates platform
    // input into engine UI vocabulary. Option (3) — engine owns a `ui::host` — is then "move this
    // function", not a redesign. Do not spread `String::Input` reads into other UI code.
    string::ui::interaction_input populate_interaction(float delta_time);
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

    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;

    // Shape overlay (rounded-rect SDF): ui_shader.slang. Pipeline owned by the hot-reloadable
    // shader_program (recompiles + swaps on save); the pass binds shape_program_->current().
    string::gpu::shader_program* shape_program_ = nullptr;
    std::vector<Ring> shape_ring_;

    // Text overlay (glyph SDF sampling the atlas): text_shader.slang.
    string::gpu::shader_program* text_program_ = nullptr;
    std::vector<Ring> glyph_ring_;
    std::vector<std::vector<DrawBatch>> batches_;   // per frame-in-flight, parallel to the rings
    string::gpu::resource_id atlas_image_ = 0;
    VkSampler atlas_sampler_ = VK_NULL_HANDLE;
    std::uint32_t atlas_slot_ = 0;
    // Dynamic-atlas re-upload: a host-visible staging buffer per frame in flight (the atlas grows as
    // new glyphs appear). record_compute copies the whole CPU atlas into this frame's staging and
    // vkCmdCopyBufferToImage's it when the atlas reports dirty, wrapped in the SHADER_READ<->TRANSFER
    // barriers. atlas_uploaded_ gates the very first transition (UNDEFINED -> SHADER_READ).
    std::vector<string::gpu::resource_id> atlas_staging_;
    std::uint32_t atlas_upload_bytes_ = 0;
    bool atlas_uploaded_ = false;

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

}  // namespace sandbox
