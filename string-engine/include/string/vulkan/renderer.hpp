#pragma once

#include <memory>
#include <string/vulkan/frame.hpp>
#include <string/vulkan/pipelines/pipeline_2d.hpp>
#include <string/vulkan/pipelines/pipeline_3d.hpp>
#include <string/vulkan/pipelines/pipeline_grid_2d.hpp>
#include <string/vulkan/pipelines/hello_slang_pipeline.hpp>
#include <string/vulkan/command_recorder.hpp>
#include <string/vulkan/driver.hpp>
#include <string/vulkan/presenter.hpp>
#include <string/vulkan/queue.hpp>
#include <string/vulkan/resource.hpp>
#include <string/vulkan/resource_allocator.hpp>
#include <string/vulkan/descriptor_allocator.hpp>
#include <string/vulkan/passes/hello_triangle_pass.hpp>
#include <string/vulkan/passes/grid_2d_pass.hpp>
#include <string/vulkan/passes/geometry_pass.hpp>
#include <string/vulkan/passes/composite_pass.hpp>

#include <volk.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string/core/logger.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include <string/platform/window.hpp>
#include <string/vulkan/device.hpp>
#include <string/vulkan/render_data.hpp>
#include <string/core/app_info.hpp>
#include <string/scene.hpp>

namespace String
{

class Renderer
{
    static constexpr uint32_t frames_in_flight_ = 3;
    ApplicationInfo application_info_;
    std::shared_ptr<Window> window_;
    Driver driver_;
    Device device_;
    Queue graphics_queue_;
    Queue compute_queue_;
    Presenter presenter_;
    ResourceAllocator allocator_;
    DescriptorTable global_descriptor_table_;
    HelloTrianglePass triangle_pass_;
    Grid2DPass grid_2d_pass_;
    CompositePass composite_pass_;
    CommandRecorder transfer_command_recorder_;
    // Constructed in the ctor body: it does GPU uploads through transfer_command_recorder_,
    // which must be init()'d first, so it can't be a plain init-list member.
    std::unique_ptr<GeometryPass> geometry_pass_;
    VkSemaphore frame_semaphore_;

    // Swapchain image acquired at the start of the frame (in begin_frame), so that
    // end_rendering has a valid blit target and end_frame can submit/present against it.
    VkImage acquired_image_ = VK_NULL_HANDLE;
    VkImageView acquired_image_view_ = VK_NULL_HANDLE;
    VkSemaphore acquired_wait_semaphore_ = VK_NULL_HANDLE;
    VkSemaphore acquired_signal_semaphore_ = VK_NULL_HANDLE;
    uint64_t frame_count_ = 1;
    uint64_t current_frame_ = 0;
    std::array<Frame, frames_in_flight_> frames_;
public:
    Renderer(const ApplicationInfo& application_info, const std::shared_ptr<Window>& window);
    ~Renderer();

    void update();

    void begin_frame();
    void begin_rendering();

    void draw(Scene& scene);

    void end_rendering();
    void end_frame();

private:
    ResourceID color_attachment_;
    ResourceID depth_attachment_;

    void handle_resize(const String::View::Extent& extent);

    // Binds color_attachment_ into the bindless table as a texture and hands the composite
    // pass the global set + the slot it landed in. Called at init and after each resize.
    void bind_composite_source();
};

}  // namespace String
