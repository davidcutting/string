#pragma once

#include <vulkan/vulkan_core.h>
#include <memory>
#include <string/pipelines/pipeline_2d.hpp>
#include <string/pipelines/pipeline_3d.hpp>
#include <string/pipelines/pipeline_grid_2d.hpp>
#include <string/pipelines/hello_slang_pipeline.hpp>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string/core/logger.hpp>
#include <string/vulkan_utils.hpp>
#include <string/window.hpp>
#include <string/device.hpp>
#include <string/swapchain.hpp>
#include <string/render_data.hpp>
#include <string>
#include <vector>

namespace String {

const std::string MODEL_PATH = "./assets/viking_room.obj";
const std::string TEXTURE_PATH = "./assets/viking_room.png";

struct UIShaderConfig {
    glm::vec2 screen_size;
    glm::uint num_shapes;
    float delta_time;
};

struct UIElement {
    glm::vec4 fill;
    glm::vec4 stroke;
    glm::vec2 position;
    float radius;
    float stroke_width;
};

struct Grid2DParams {
    glm::vec4 background_color;
    glm::vec4 grid_color;
    glm::vec4 border_color;
    glm::vec4 axis_color;
    glm::vec2 grid_resolution;
    glm::vec2 grid_center;
    glm::vec2 grid_size;
    glm::vec2 screen_size;
    float line_width;
    float fade_distance;
    float border_width;
    float axis_width;
    float show_border;
    float show_axes;
};

struct Camera3D {
    alignas(16) glm::mat4 model;
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
};

struct Camera2D {
    glm::vec2 position;
    float zoom;
    float rotation;
};

struct Scene3D
{
    std::unique_ptr<Buffer> vertex_buffer;
    std::unique_ptr<Buffer> index_buffer;
    Camera3D camera;
};

struct Scene2D
{
    std::unique_ptr<Buffer> vertex_buffer;
    std::unique_ptr<Buffer> index_buffer;
    Camera2D camera;
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    void initialize(const std::shared_ptr<Window>& window);
    void update();

private:
    std::shared_ptr<Window> window_;
    std::shared_ptr<Device> device_;
    std::unique_ptr<Swapchain> swap_chain_;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    Scene3D scene_3d_;
    UIShaderConfig ui_push_constant_;
    Scene2D scene_ui_;

    std::vector<std::unique_ptr<Buffer>> camera_3d_ubo_;
    std::vector<void*> uniform_buffers_mapped_;
    std::vector<std::unique_ptr<Buffer>> camera_2d_ubo_;
    std::vector<void*> camera_2d_mapped_;
    std::vector<std::unique_ptr<Buffer>> ui_shapes_ssbo_;
    std::vector<void*> ui_elements_mapped_;

    std::vector<std::unique_ptr<Buffer>> hello_slang_buffer0_;
    std::vector<void*> hello_slang_buffer0_mapped_;
    std::vector<std::unique_ptr<Buffer>> hello_slang_buffer1_;
    std::vector<void*> hello_slang_buffer1_mapped_;
    std::vector<std::unique_ptr<Buffer>> hello_slang_result_;
    std::vector<void*> hello_slang_result_mapped_;

    VkDescriptorSetLayout descriptor_set_layout_3d_;
    std::unique_ptr<Pipeline3D> pipeline_3d_;
    VkDescriptorSetLayout ui_descriptor_set_layout_;
    VkPushConstantRange ui_push_constant_range_;
    std::unique_ptr<Pipeline2D> ui_pipeline_;
    VkPushConstantRange grid_2d_push_constant_range_;
    std::unique_ptr<PipelineGrid2D> pipeline_grid_2d_;
    // VkPushConstantRange hello_slang_pipeline_constant_range_;
    VkDescriptorSetLayout hello_slang_descriptor_set_layout_;
    std::unique_ptr<HelloSlangPipeline> hello_slang_pipeline_;

    VkDescriptorPool descriptorPool;
    VkCommandPool commandPool;

    std::unique_ptr<Image> depth_image_;
    VkImageView depthImageView;

    std::unique_ptr<Image> texture_image_;
    VkImageView textureImageView;
    VkSampler textureSampler;

    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkDescriptorSet> descriptor_sets_3d_;
    std::vector<VkDescriptorSet> ui_descriptor_sets_;
    std::vector<VkDescriptorSet> hello_slang_descriptor_sets_;
    std::vector<VkDescriptorSet> grid_2d_descriptor_sets_;

    uint32_t swap_chain_image_count_{2};
    uint32_t current_frame = 0;
    std::vector<VkFence> frame_in_flight_fences_;
    std::vector<VkSemaphore> image_available_semaphores_;
    std::vector<VkSemaphore> render_complete_semaphores_;

    bool framebufferResized = false;

    void framebuffer_resize_callback(const String::View::Extent& /*extent*/) { framebufferResized = true; }

    void cleanup();

    void recreateSwapChain();

    void createDescriptorSetLayout();

    void createCommandPool();

    void createDepthResources();

    bool hasStencilComponent(VkFormat format);

    void createTextureImage();

    void createTextureImageView();

    void createTextureSampler();

    VkImageView createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags);

    void createImage(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
                     VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& imageMemory);

    void transitionImageLayout(VkImage image, VkFormat /*format*/, VkImageLayout oldLayout, VkImageLayout newLayout);

    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);

    void create_vertex_buffer();

    void create_index_buffer();

    void createUniformBuffers();

    void create_ssbo_buffer();

    void createDescriptorPool();

    void createDescriptorSets();

    VkCommandBuffer beginSingleTimeCommands();

    void endSingleTimeCommands(VkCommandBuffer commandBuffer);

    void copy_buffer(const Buffer* src, const Buffer* dest);

    void createCommandBuffers();

    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);

    void createSyncObjects();

    void updateUniformBuffer(uint32_t currentImage);

    void drawFrame();
};

}  // namespace String
