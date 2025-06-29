#pragma once

#include <string/window.hpp>
#include <string/device.hpp>
#include <string/swapchain.hpp>
#include <memory>

namespace String
{

struct PerFrame
{
    
};

class Renderer2D
{
public:
    Renderer2D();

private:
    std::array<PerFrame, 2> per_frame_resources_;
    std::shared_ptr<Window> window_;
    std::shared_ptr<Device> device_;
    std::unique_ptr<Swapchain> swapchain_;
};

}
