#pragma once

#include <filesystem>

#include <string/vulkan/render_pass.hpp>

#include <volk.h>

namespace String
{

class HelloTrianglePass final : public Pass
{
    Device& device_;

public:
    HelloTrianglePass(Device& device, const std::filesystem::path& resources_path, const uint16_t& frames_in_flight);
    virtual ~HelloTrianglePass() override;

    virtual void update(const float& delta_time, const uint16_t& current_frame) override;
    virtual void record(CommandRecorder& recorder, const uint16_t& current_frame) override;
};

}