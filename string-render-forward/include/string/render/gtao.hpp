#pragma once

#include <cstdint>

#include <string/gpu/pass_context.hpp>
#include <string/gpu/resource.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/frame_graph.hpp>

#include <string/render/scene_bridge.hpp>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

namespace string::render
{

// Ground-truth ambient occlusion + bent normals, half res, temporally reprojected. Brief 20: a plain
// app-owned object — no base class, no owned images, no bindless slot vectors, no sampler.
//
// It is TWO passes, because it is two dispatches with a producer/consumer edge between them:
//
//   gtao.raw       reprojects the previous frame's depth into a raw half-res AO + bent normal
//   gtao.denoise   reads that raw target and writes the final AO the lit shader samples
//
// Declaring them separately is what deleted all three hand-rolled transitions. The raw target's
// GENERAL -> SHADER_READ_ONLY hand-over is now the derived barrier between two declared passes; the
// first-frame UNDEFINED -> GENERAL transition is the discard the graph derives for a first write; and
// the cross-frame write-after-read (last frame's denoise sampled `raw`, this frame's main writes it)
// derives too, because tracked state carries across the frame boundary. `raw` is a single image, not
// a ring, and so is the final target — the inventory proved neither is temporal.
//
// The ONE genuine temporal dependency is the input: reprojection reads the PREVIOUS frame slot's
// resolved depth. That is expressed as a distinct logical handle over the same physical ring, rotated
// one slot back, which the application declares and hands to declare(). Nothing here indexes a ring.
class gtao_chain
{
public:
    gtao_chain(string::engine_context& ctx, const scene_bridge* bridge);
    ~gtao_chain();

    gtao_chain(const gtao_chain&) = delete;
    gtao_chain& operator=(const gtao_chain&) = delete;

    // Author both passes. `depth_prev` is the depth-history ring ROTATED ONE SLOT BACK, so resolving
    // it for the current frame slot yields the previous frame's resolved depth — the reprojection
    // source. `raw` and `ao` are the two half-res targets.
    void declare(::string::frame_graph& fg, ::string::gpu::image depth_prev,
                 ::string::gpu::image raw, ::string::gpu::image ao);

    // Ordinary per-frame CPU work the app calls before the graph executes: recomputes the half-res
    // size for `extent` and decides whether the chain runs this frame (the cvars, the furnace gate,
    // and whether the previous slot's depth history is valid). Must run before anything reads
    // runs()/size() — GeometryPass's SceneData does.
    void tick(VkExtent2D extent, uint16_t current_frame);

    bool runs() const { return runs_this_frame_; }
    // Has the chain EVER produced output? gtao.ao's contents persist across idle frames, but until
    // the first run there is nothing to sample — and no constant neutral can stand in for an
    // ENCODED bent normal (0.5s decode to the zero vector, which nukes the sun/spec terms). The
    // shader's gtao_slot == ~0u branch is the only sound degrade, so SceneData must take it until
    // this is true.
    bool has_output() const { return has_output_; }
    glm::uvec2 size() const { return size_; }
    // The depth-history slot this frame reprojects FROM (the previous frame in flight). Published so
    // the application can rotate the depth-history handle's backing by the same rule.
    uint16_t prev_slot(uint16_t current_frame) const;

private:
    void record_raw(::string::pass_context& ctx, ::string::gpu::image depth_prev,
                    ::string::gpu::image raw);
    void record_denoise(::string::pass_context& ctx, ::string::gpu::image depth_prev,
                        ::string::gpu::image raw, ::string::gpu::image ao);
    // The shared half of both push blocks: the reprojection matrices and sizes captured with the
    // previous slot's depth. False when that history is not usable.
    bool base_push(::string::pass_context& ctx, GtaoPush& out) const;
    void dispatch(::string::pass_context& ctx, ::string::gpu::shader_program* prog,
                  const GtaoPush& push) const;

    ::string::gpu::device* device_ = nullptr;
    ::string::gpu::descriptor_table* descriptors_ = nullptr;
    const scene_bridge* bridge_ = nullptr;
    uint32_t frames_in_flight_ = 0;

    glm::uvec2 size_{ 0, 0 };
    ::string::gpu::shader_program* program_ = nullptr;
    ::string::gpu::shader_program* denoise_program_ = nullptr;
    bool runs_this_frame_ = false;
    bool has_output_ = false;   // sticky: set on the chain's first surviving run
};

}  // namespace string::render
