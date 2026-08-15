// shadow cascade fit, HiZ pyramid, GTAO, local-light stress — split out of geometry_pass.cpp (brief 11 modularization). These remain geometry_pass
// member functions (cohesive translation-unit split; the geometry core stays one class, its true
// graph-pass decoupling is Phase 2). State lives in geometry_pass.hpp.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <string/core/logger.hpp>
#include <string/gpu/command_recorder.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/gpu/shader_compiler.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <string/vulkan/passes/composite_pass.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <string/render/geometry_pass.hpp>
#include <string/render/render_cvars.hpp>

namespace string::render
{
using namespace string;

// Brief 20: this used to allocate the depth + pyramid rings, create per-mip views, claim bindless
// slots and build a sampler. All of that is the graph's now — the pyramid is an application-declared
// transient and every slot comes from ctx.slot(pyramid.mip(m)) while the pass records. What is left
// is the SHAPE, which the technique still needs to size its dispatches.
void geometry_pass::ensure_hiz(uint16_t current_frame)
{
    (void)current_frame;
    if (screen_size.width == 0 || screen_size.height == 0) return;
    const VkExtent2D base = hiz_extent(screen_size);
    if (base.width == hiz_screen_w_ && base.height == hiz_screen_h_) return;
    hiz_screen_w_ = base.width;
    hiz_screen_h_ = base.height;

    // The pyramid IMAGE follows the viewport by declaration now (viewport_fit::half_pow2 + all_mips,
    // brief 21 step 2), so the dispatch shape is just the same relationship computed here — no clamp,
    // because there is no longer a fixed declared extent to outgrow.
    const uint32_t mips = hiz_mip_count(screen_size);
    hiz_.assign(frames_in_flight_, HizPyramid{ mips, glm::uvec2(base.width, base.height) });
    STRING_LOG_INFO("[hiz] pyramid {}x{}, {} mips", base.width, base.height, mips);

    // The resolved-depth contents every slot held are gone with the resize, so GTAO's reprojection
    // input is invalid until each slot is rendered again. SIZING is load-bearing: the brief-20
    // rewrite deleted the hiz ring allocation that used to size this vector, leaving it EMPTY —
    // GTAO's `prev < bridge_.depth_history().size()` readiness check then never passed, GTAO silently never
    // ran, and its consumers sampled the 0.5 neutral fallback whose decoded bent normal is the
    // zero vector — the giant pose-dependent black regions that presented as broken shadows.
    bridge_.depth_history().assign(frames_in_flight_, DepthHistorySlot{});
}




// The HiZ pyramid's shape, as a pure function of the viewport. Both the application (declaring the
// transient) and geometry_pass::declare (authoring one pass per mip) must agree on this, and
// authored-once means they compute it ONCE from the initial viewport rather than per frame. Mip 0 is
// half of the next power of two, which keeps the reduction exact at every level.
VkExtent2D geometry_pass::hiz_extent(VkExtent2D viewport)
{
    const auto next_pow2 = [](uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; };
    return { std::max(1u, next_pow2(viewport.width) / 2), std::max(1u, next_pow2(viewport.height) / 2) };
}

uint32_t geometry_pass::hiz_mip_count(VkExtent2D viewport)
{
    const VkExtent2D base = hiz_extent(viewport);
    return static_cast<uint32_t>(std::floor(std::log2(std::max(base.width, base.height)))) + 1;
}

// The per-frame parameter blocks the sky, froxel and IBL components latch. Every field is already
// scene state; this is assembly, not policy.



std::size_t geometry_pass::gi_capture_entries() const
{
    if (draw_info_mapped_ == nullptr) return 0;
    return probe_gi_component::capture_table({ draw_info_mapped_, draw_count_ }).size();
}

}  // namespace string::render
