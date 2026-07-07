#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
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

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <volk.h>

namespace sandbox
{

// Push constant for the 3D pipeline: precomputed model-view-projection plus the bindless
// slot of the model's texture. Layout must match shaders/3d_shader.{vert,frag}.
struct GeometryPush {
    glm::mat4 mvp;
    uint32_t texture_slot;
};

struct Scene3D
{
    String::ResourceID vertex_buffer;
    String::ResourceID index_buffer;
    uint32_t index_count;
};

class GeometryPass final : public String::Pass
{
    String::Device& device_;
    String::ResourceAllocator& allocator_;
    String::DescriptorTable& descriptor_table_;
    Scene3D scene_3d_;
    String::Pipeline pipeline_;

    String::ResourceID texture_image_;
    uint32_t texture_slot_ = 0;
    GeometryPush push_{};

public:
    // The mesh (.obj) and texture are resolved relative to context.resources_path — the
    // application supplies them as content, so no asset is baked into the library.
    GeometryPass(
        String::PassContext& context,
        const std::filesystem::path& model_path,
        const std::filesystem::path& texture_path);
    virtual ~GeometryPass() override;

    virtual void update(float delta_time, uint16_t current_frame) override;
    virtual void record(String::CommandRecorder& recorder, uint16_t current_frame) override;
};

}  // namespace sandbox
