#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <string/vulkan/command_recorder.hpp>
#include <string/vulkan/device.hpp>
#include <string/vulkan/render_data.hpp>
#include <string/vulkan/render_pass.hpp>
#include <string/vulkan/pipelines/pipeline_3d.hpp>
#include "string/vulkan/descriptor_allocator.hpp"
#include "string/vulkan/resource.hpp"
#include "string/vulkan/resource_allocator.hpp"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <volk.h>

namespace String
{

const std::string MODEL_PATH = "assets/viking_room.obj";
const std::string TEXTURE_PATH = "assets/viking_room.png";

// Push constant for the 3D pipeline: precomputed model-view-projection plus the bindless
// slot of the model's texture. Layout must match shaders/3d_shader.{vert,frag}.
struct GeometryPush {
    glm::mat4 mvp;
    uint32_t texture_slot;
};

struct Scene3D
{
    ResourceID vertex_buffer;
    ResourceID index_buffer;
    uint32_t index_count;
};

class GeometryPass final : public Pass
{
    Device& device_;
    ResourceAllocator& allocator_;
    DescriptorTable& descriptor_table_;
    Scene3D scene_3d_;
    std::unique_ptr<Pipeline3D> pipeline_3d_;

    ResourceID texture_image_;
    uint32_t texture_slot_ = 0;
    GeometryPush push_{};

public:
    GeometryPass(
        Device& device,
        ResourceAllocator& allocator,
        DescriptorTable& descriptor_table,
        CommandRecorder& streaming_recorder,
        const std::filesystem::path& resources_path,
        const uint16_t& frames_in_flight);
    virtual ~GeometryPass() override;

    virtual void update(const float& delta_time, const uint16_t& current_frame) override;
    virtual void record(CommandRecorder& recorder, const uint16_t& current_frame) override;
};

}
