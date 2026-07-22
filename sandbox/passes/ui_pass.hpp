#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <string/core/font.hpp>
#include <string/core/layout.hpp>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <string/gpu/resource.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/platform/input.hpp>
#include <string/vulkan/pass_context.hpp>
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
        const String::Input& input;
        std::uint64_t hovered;  // id.hash under the cursor (as of last frame's layout); 0 = none
        std::uint64_t focused;  // id.hash of the focused element; 0 = none
    };
    // Authors the frame's UI into a builder already opened at a screen-filling root container.
    using Author = std::function<void(string::layout_builder&, const UiContext&)>;

    // `atlas` is the SDF font atlas (baked once, shared with nothing else). `author` declares the
    // whole UI (shapes + text) each frame.
    UIPass(String::PassContext& context, std::shared_ptr<const string::font_atlas> atlas,
           Author author);
    ~UIPass() override;

    UIPass(const UIPass&) = delete;
    UIPass& operator=(const UIPass&) = delete;

    void update(float delta_time, uint16_t current_frame) override;
    void record(string::gpu::command_recorder& recorder, uint16_t current_frame) override;

private:
    // Per-frame ring capacities. Content beyond these is dropped (with a one-time warn).
    static constexpr std::uint32_t kMaxShapes = 4096;
    static constexpr std::uint32_t kMaxGlyphs = 8192;

    // A persistent-mapped storage buffer per frame in flight (the geometry_streamer pattern).
    struct Ring
    {
        string::gpu::resource_id buffer = 0;
        std::uint32_t slot = 0;
        void* mapped = nullptr;
        std::uint32_t count = 0;
    };
    // Create `frames_in_flight` mapped storage buffers of `capacity * stride` bytes, bound bindless.
    void make_ring(std::vector<Ring>& ring, std::uint32_t frames_in_flight, std::uint32_t capacity,
                   std::size_t stride);

    string::gpu::device& device_;
    string::gpu::resource_allocator& allocator_;
    string::gpu::descriptor_table& descriptor_table_;

    std::shared_ptr<const string::font_atlas> atlas_;
    Author author_;
    String::Input& input_;  // non-const: the pass requests game/UI capture mode
    // Read each frame for the shader-compile error overlay (brief 01, M4): when a hot-reload fails,
    // the registry holds the diagnostics; the pass draws them over the UI until the next success.
    string::gpu::shader_program_registry& shader_registry_;
    string::layout_builder builder_;
    std::uint64_t hovered_id_ = 0;
    std::uint64_t focused_id_ = 0;
    bool prev_click_ = false;  // for left-button edge detection in UI mode

    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;

    // Shape overlay (rounded-rect SDF): ui_shader.slang. Pipeline owned by the hot-reloadable
    // shader_program (recompiles + swaps on save); the pass binds shape_program_->current().
    string::gpu::shader_program* shape_program_ = nullptr;
    std::vector<Ring> shape_ring_;

    // Text overlay (glyph SDF sampling the atlas): text_shader.slang.
    string::gpu::shader_program* text_program_ = nullptr;
    std::vector<Ring> glyph_ring_;
    string::gpu::resource_id atlas_image_ = 0;
    VkSampler atlas_sampler_ = VK_NULL_HANDLE;
    std::uint32_t atlas_slot_ = 0;

    bool warned_overflow_ = false;

    // Persistent backing for the compile-error overlay's text (add_text takes non-owning views, so
    // the strings must outlive layout + record). Rebuilt each frame from the registry's diagnostics.
    std::vector<std::string> error_lines_;
    // Appends the shader-compile error overlay to the current layout tree, if any errors are live.
    void author_error_overlay();
};

}  // namespace sandbox
