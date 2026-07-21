#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/device.hpp>
#include <string/vulkan/render_data.hpp>
#include <string/vulkan/render_pass.hpp>
#include <string/vulkan/pass_context.hpp>
#include <string/scene/camera.hpp>
#include <string/gpu/pipeline.hpp>
#include "string/gpu/descriptor_allocator.hpp"
#include "string/gpu/residency_manager.hpp"
#include "string/gpu/resource.hpp"
#include "string/gpu/resource_allocator.hpp"

#include "gltf_loader.hpp"
#include "geometry_streamer.hpp"
#include "texture_streamer.hpp"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <volk.h>

namespace sandbox
{

// Push constant for the 3D pipeline: the camera's view-projection (per frame), the device address
// of the vertex SSBO the shader pulls from (programmable vertex pulling), and the bindless slot of
// the per-draw data buffer. Field order/layout must match the Push block in shaders/3d_shader.vert
// (mat4 at 0, 8-byte device address at 64, uint at 72).
struct GeometryPush
{
    glm::mat4 view_proj;
    VkDeviceAddress vertex_address;
    uint32_t drawdata_slot;
};

// Per-draw record read by the vertex shader (indexed by gl_InstanceIndex). Baked at load: the
// node transform, the material's base-color factor, and the bindless slot of its base-color
// texture. Padded to a 16-byte-aligned stride to match the shader's std430 DrawData[] layout.
struct DrawData
{
    glm::mat4 model;
    glm::vec4 base_color;
    uint32_t texture_slot;
    uint32_t _pad[3];
};

// Per-draw input to the GPU cull compute shader (scalar layout): world-space AABB + the fields
// needed to emit this draw's VkDrawIndexedIndirectCommand. Matches CullDraw in cull.comp.
struct CullDraw
{
    glm::vec4 aabb_min;
    glm::vec4 aabb_max;
    uint32_t index_count;
    uint32_t first_index;
    uint32_t vertex_offset;
    uint32_t draw_id;
};

// Push constant for cull.comp: the camera view-projection (for frustum extraction), the device
// addresses of the cull input + indirect output buffers, and the draw count. Padded so its size
// matches the shader block's alignment.
struct CullPush
{
    glm::mat4 view_proj;
    VkDeviceAddress cull_in;
    VkDeviceAddress indirect_out;
    uint32_t draw_count;
    uint32_t cull_enabled;
};

// Renders a whole glTF model: uploads its shared vertex/index buffers plus every texture (each
// into a bindless slot), then issues one indexed draw per node-instanced primitive, pushing the
// primitive's transform and material inline. Content-agnostic — the model path is supplied by
// the application, resolved under context.resources_path.
class GeometryPass final : public String::Pass
{
    string::gpu::device& device_;
    string::gpu::resource_allocator& allocator_;
    string::gpu::descriptor_table& descriptor_table_;
    String::InputMap& input_map_;
    string::gpu::pipeline pipeline_;
    string::gpu::pipeline cull_pipeline_;

    string::gpu::resource_id vertex_buffer_;
    string::gpu::resource_id index_buffer_;
    // GPU-driven draw state, all built once at load: the per-draw data SSBO (bound into the
    // bindless table at draw_data_slot_) for the vertex shader.
    string::gpu::resource_id draw_data_buffer_;
    uint32_t draw_data_slot_ = 0;
    uint32_t draw_count_ = 0;
    // GPU culling buffers (device-addressed): the compute shader reads cull_draw_buffer_ (AABBs +
    // command fields, static/shared) and writes one indirect command per draw into
    // culled_indirect_buffer_ each frame — instanceCount 1 (visible) or 0 (culled) — which
    // record() then issues with vkCmdDrawIndexedIndirect. The output is rewritten every frame, so
    // there's one per frame-in-flight to avoid a frame N+1 compute clobbering the buffer frame N's
    // draw is still reading on the GPU.
    string::gpu::resource_id cull_draw_buffer_;
    // Host-visible mapped view of cull_draw_buffer_: the geometry streamer flips each draw's
    // index_count here (0 = not resident / not drawn, real = resident) as geometry streams in.
    CullDraw* cull_draw_mapped_ = nullptr;
    std::vector<string::gpu::resource_id> culled_indirect_buffers_;
    uint32_t frames_in_flight_ = 1;

    // Geometry residency streaming: per-draw vertex/index ranges suballocate into heaps SMALLER than
    // the whole model as draws enter the view frustum, and are freed (reclaimed) on eviction. A
    // not-yet-resident draw stays hidden (index_count 0). Capping resident geometry below the full
    // model is what exercises reclaim; raise toward 100 for a VRAM-fitting scene with no pop-in.
    static constexpr std::uint64_t kGeometryResidentPercent = 100;
    static constexpr VkDeviceSize kGeometryStreamPerFrame = 32ull * 1024 * 1024;
    // Per-model budget (heap capacity), so this is a unique_ptr built once sizes are known.
    std::unique_ptr<string::gpu::residency_manager> geometry_residency_;
    std::unique_ptr<GeometryStreamer> geometry_streamer_;
    // Only stream per-frame when the model doesn't fit the budget (< 100%). When it fits, all
    // geometry is uploaded up front (no per-frame streaming/eviction cost) — streaming a scene that
    // fits in VRAM just adds startup lag for no benefit.
    bool geometry_streaming_ = false;

    // Per glTF-image backing resource + its bindless slot (index-aligned with the loaded
    // model's textures, so a material's texture index maps straight to a slot).
    std::vector<string::gpu::resource_id> texture_images_;
    std::vector<uint32_t> texture_slots_;
    // 1x1 white fallback, used for draws whose material has no base-color texture (the
    // base-color factor still tints it).
    string::gpu::resource_id white_image_;
    uint32_t white_slot_ = 0;

    // Texture residency streaming: the streamer (a residency_provider) owns each texture's mip
    // levels and adjustable-minLod sampler; the manager drives what streams in per frame. Only
    // cooked KTX2 textures stream — stb-decoded fallbacks upload whole as before. Budget is large
    // (no physical VRAM reclaim yet — see TextureStreamer), so all wanted detail streams in.
    static constexpr VkDeviceSize kTextureBudget = 6ull * 1024 * 1024 * 1024;
    // Cap new streaming per frame so the coarse scene appears instantly and sharpens over ~a second,
    // rather than stalling frame 0 on the whole fine-mip upload.
    static constexpr VkDeviceSize kTextureStreamPerFrame = 64ull * 1024 * 1024;
    // Coarse-mip LOD floor: the coarsest resident mip every streamed texture is pinned to is roughly
    // this wide, so the whole scene renders blurry-but-real up front and only visible surfaces pull
    // finer mips (screen-coverage feedback in update()).
    static constexpr std::uint32_t kCoarseFloorPixels = 64;
    string::gpu::residency_manager residency_;
    std::unique_ptr<TextureStreamer> texture_streamer_;
    std::vector<string::gpu::resource_id> streamed_textures_;
    // Per glTF texture (index-aligned with texture_images_): the info the coverage heuristic needs
    // to pick a desired mip. levels == 0 marks a non-streamed (stb) texture the feedback skips.
    struct TextureLod
    {
        std::uint32_t levels = 0;         // total mip levels
        std::uint32_t base_extent = 0;    // max(width, height) of mip 0
        std::uint32_t coarse_detail = 0;  // pinned coarse-tail detail (never wanted below this)
    };
    std::vector<TextureLod> texture_lod_;
    // Scratch reused each frame: the finest detail any visible draw wants per texture (starts at the
    // coarse floor, raised by coverage), issued as one want() per texture after the draw loop.
    std::vector<std::uint32_t> frame_desired_detail_;
    uint64_t stream_frame_ = 0;
    bool logged_full_resident_ = false;

    std::vector<GltfMaterial> materials_;
    std::vector<GltfDraw> draws_;

    // Reusable engine fly camera (glTF space, Y-up). Framed to the model's AABB at load; update()
    // drives it through the InputMap's default fly controls + mouse-look.
    String::Camera camera_;

    // Debug: freeze the culling frustum (toggled by the "freeze_culling" action). While frozen,
    // the cull compute shader tests against frozen_cull_view_proj_ instead of the live camera, so
    // moving the camera reveals what culling removes (geometry outside the frozen view vanishes).
    bool cull_frozen_ = false;
    glm::mat4 frozen_cull_view_proj_{ 1.0f };
    // Debug: toggle GPU frustum culling off entirely (C) — for isolating culling from geometry.
    bool cull_enabled_ = true;

public:
    // model_path is a .gltf/.glb resolved relative to context.resources_path.
    GeometryPass(String::PassContext& context, const std::filesystem::path& model_path);
    virtual ~GeometryPass() override;

    virtual void update(float delta_time, uint16_t current_frame) override;
    virtual bool record_compute(string::gpu::command_recorder& recorder, uint16_t current_frame) override;
    virtual void record(string::gpu::command_recorder& recorder, uint16_t current_frame) override;
};

}  // namespace sandbox
