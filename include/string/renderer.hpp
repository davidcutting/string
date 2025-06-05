#pragma once

#include <vulkan/vulkan_core.h>
#include <memory>
#include "string/pipeline_2d.hpp"
#include "string/pipeline_3d.hpp"
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
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

struct Camera3D {
    alignas(16) glm::mat4 model;
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
};

struct Camera2D {
    alignas(16) glm::mat4 model;
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
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
    void initialize(const std::shared_ptr<Window>& window) {
        // Window
        window->register_resize_event_callback(
            std::bind(&Renderer::framebuffer_resize_callback, this, std::placeholders::_1));
        window_ = window;

        // Device
        device_ = std::make_shared<Device>(window);
        VkExtent2D extent = {
            .width = window->get_properties().extent.width,
            .height = window->get_properties().extent.height
        };
        swap_chain_ = std::make_unique<Swapchain>(device_, extent);
        swap_chain_image_count_ = swap_chain_->get_swap_chain_image_count();

        createDescriptorSetLayout();
        pipeline_3d_ = std::make_unique<Pipeline3D>(device_, descriptorSetLayout);

        createCommandPool();
        createDepthResources();

        createTextureImage();
        createTextureImageView();
        createTextureSampler();
        vku::load_model(MODEL_PATH, vertices, indices);
        create_vertex_buffer();
        create_index_buffer();
        createUniformBuffers();

        createDescriptorPool();
        createDescriptorSets();
        createCommandBuffers();
        createSyncObjects();
    }

    void update() { drawFrame(); }

    ~Renderer() {
        vkDeviceWaitIdle(device_->get_device());
        cleanup();
    }

private:
    std::shared_ptr<Window> window_;
    std::shared_ptr<Device> device_;
    std::unique_ptr<Swapchain> swap_chain_;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    Scene3D scene_3d_;
    Scene2D scene_ui_;

    std::vector<std::unique_ptr<Buffer>> uniform_buffers_;
    std::vector<void*> uniform_buffers_mapped_;

    VkDescriptorSetLayout descriptorSetLayout;
    std::unique_ptr<Pipeline3D> pipeline_3d_;
    std::unique_ptr<Pipeline2D> ui_pipeline_;

    VkCommandPool commandPool;

    std::unique_ptr<Image> depth_image_;
    VkImageView depthImageView;

    std::unique_ptr<Image> texture_image_;
    VkImageView textureImageView;
    VkSampler textureSampler;

    VkDescriptorPool descriptorPool;
    std::vector<VkDescriptorSet> descriptorSets;

    std::vector<VkCommandBuffer> commandBuffers;

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
