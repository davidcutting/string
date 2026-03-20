#include <string/vulkan/passes/hello_triangle_pass.hpp>

namespace String
{

HelloTrianglePass::HelloTrianglePass(Device& device, const std::filesystem::path& resources_path, const uint16_t& frames_in_flight)
: device_(device)
{

}

HelloTrianglePass::~HelloTrianglePass()
{

}

void HelloTrianglePass::update(const float& delta_time, const uint16_t& current_frame)
{

}

void HelloTrianglePass::record(CommandRecorder& recorder, const uint16_t& current_frame)
{

}

}