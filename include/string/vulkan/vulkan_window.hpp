// MIT License
//
// Copyright (c) 2022 David Cutting
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <bits/stdc++.h>
#include <vulkan/vulkan.h>

#include <string/window.hpp>
#include "string/core/debug.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace String {
namespace Vulkan {

class VulkanWindow final : String::Window {
private:
    GLFWwindow* window_handle_;

public:
    VulkanWindow(const Properties& properties) noexcept;
    ~VulkanWindow() noexcept;

    virtual void update() const noexcept override final;

    virtual bool is_open() const noexcept override final;
    virtual void* get_native_window() const noexcept override final;

    // TODO: Is this the best place to do this? Maybe VulkanInstance or VulkanContext is better?
    std::optional<VkSurfaceKHR> create_window_surface(const VkInstance& instance) const noexcept;
};  // class VulkanWindow

inline VulkanWindow::VulkanWindow(const Window::Properties& properties) noexcept : String::Window(properties) {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window_handle_ = glfwCreateWindow(_window_properties.extent.width, _window_properties.extent.height,
                                      _window_properties.title.c_str(), nullptr, nullptr);

    glfwSetWindowUserPointer(window_handle_, this);
}

inline VulkanWindow::~VulkanWindow() noexcept {
    glfwDestroyWindow(window_handle_);
    glfwTerminate();
}

inline void VulkanWindow::update() const noexcept { glfwPollEvents(); }

inline bool VulkanWindow::is_open() const noexcept { return !glfwWindowShouldClose(window_handle_); }

inline void* VulkanWindow::get_native_window() const noexcept { return window_handle_; }

inline std::optional<VkSurfaceKHR> VulkanWindow::create_window_surface(const VkInstance& instance) const noexcept {
    VkSurfaceKHR surface;

    if (glfwCreateWindowSurface(instance, window_handle_, nullptr, &surface) != VK_SUCCESS) {
        STRING_LOG_WARN("Failed to create window surface!");
        return std::nullopt;
    }
    return surface;
}

}  // namespace Vulkan
}  // namespace String