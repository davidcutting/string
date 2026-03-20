#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include <string/vulkan/allocator.hpp>
#include <string/vulkan/render_pass.hpp>
#include <string/vulkan/pipelines/hello_slang_pipeline.hpp>
#include <string/vulkan/device.hpp>


#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <volk.h>

namespace String
{

class HelloSlang final : public Pass
{
    Device& device_;
    std::unique_ptr<HelloSlangPipeline> hello_slang_pipeline_;

    std::vector<std::unique_ptr<Buffer>> hello_slang_buffer0_;
    std::vector<std::span<float>> hello_slang_buffer0_mapped_;
    std::vector<std::unique_ptr<Buffer>> hello_slang_buffer1_;
    std::vector<std::span<float>> hello_slang_buffer1_mapped_;
    std::vector<std::unique_ptr<Buffer>> hello_slang_result_;
    std::vector<std::span<float>> hello_slang_result_mapped_;

public:
    HelloSlang(Device& device, const std::filesystem::path& resources_path, const uint16_t& frames_in_flight);
    virtual ~HelloSlang() override;

    virtual void update(const float& delta_time, const uint16_t& current_frame) override;
    virtual void record(CommandRecorder& recorder, const uint16_t& current_frame) override;
};

}