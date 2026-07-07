#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <string/vulkan/command_recorder.hpp>
#include <string/vulkan/device.hpp>
#include <string/vulkan/render_data.hpp>
#include <string/vulkan/render_pass.hpp>
#include <string/vulkan/pass_context.hpp>
#include <string/vulkan/pipeline.hpp>
#include "string/vulkan/descriptor_allocator.hpp"
#include "string/vulkan/resource.hpp"
#include "string/vulkan/resource_allocator.hpp"

#include "gltf_loader.hpp"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <volk.h>

namespace sandbox
{

// Push constant for the 3D pipeline: per-draw model-view-projection, the material's base-color
// factor, and the bindless slot of its base-color texture. Layout must match
// shaders/3d_shader.{vert,frag}.
struct GeometryPush
{
    glm::mat4 mvp;
    glm::vec4 base_color;
    uint32_t texture_slot;
};

// Renders a whole glTF model: uploads its shared vertex/index buffers plus every texture (each
// into a bindless slot), then issues one indexed draw per node-instanced primitive, pushing the
// primitive's transform and material inline. Content-agnostic — the model path is supplied by
// the application, resolved under context.resources_path.
class GeometryPass final : public String::Pass
{
    String::Device& device_;
    String::ResourceAllocator& allocator_;
    String::DescriptorTable& descriptor_table_;
    const String::Input& input_;
    String::Pipeline pipeline_;

    String::ResourceID vertex_buffer_;
    String::ResourceID index_buffer_;

    // Per glTF-image backing resource + its bindless slot (index-aligned with the loaded
    // model's textures, so a material's texture index maps straight to a slot).
    std::vector<String::ResourceID> texture_images_;
    std::vector<uint32_t> texture_slots_;
    // 1x1 white fallback, used for draws whose material has no base-color texture (the
    // base-color factor still tints it).
    String::ResourceID white_image_;
    uint32_t white_slot_ = 0;

    std::vector<GltfMaterial> materials_;
    std::vector<GltfDraw> draws_;

    // Fly camera (glTF space, Y-up). Initial pose + speed/near/far are derived from the model's
    // AABB at load; update() then drives it from input (WASD + mouse-look).
    glm::vec3 camera_pos_{ 0.0f };
    float camera_yaw_ = 0.0f;     // radians about +Y
    float camera_pitch_ = 0.0f;   // radians, clamped to +/-89 deg
    float move_speed_ = 1.0f;     // units/sec, scaled to model size
    float camera_near_ = 0.1f;
    float camera_far_ = 100.0f;
    glm::mat4 view_proj_{ 1.0f };

public:
    // model_path is a .gltf/.glb resolved relative to context.resources_path.
    GeometryPass(String::PassContext& context, const std::filesystem::path& model_path);
    virtual ~GeometryPass() override;

    virtual void update(float delta_time, uint16_t current_frame) override;
    virtual void record(String::CommandRecorder& recorder, uint16_t current_frame) override;

private:
    // Uploads one decoded texture into a device-local image, binds it into the bindless table,
    // and returns {image resource, slot}. Format is chosen from the texture's sRGB flag.
    void upload_texture(String::PassContext& context, const GltfTexture& texture,
                        String::ResourceID& out_image, uint32_t& out_slot);
};

}  // namespace sandbox
