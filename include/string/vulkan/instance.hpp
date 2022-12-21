// MIT License
//
// Copyright (c) 2020-2023 David Cutting
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
#include <vulkan/vulkan_core.h>

#include <string/core/version.hpp>

namespace String {
namespace Vulkan {

class Instance {
public:
    Instance(const std::string& application_name, const Version& application_version,
             const std::vector<const char*>& required_extensions = {},
             const std::vector<const char*>& required_layers = {});
    Instance(VkInstance instance);
    Instance(const Instance&) = delete;
    Instance(Instance&&) = delete;
    ~Instance();
    Instance& operator=(const Instance&) = delete;
    Instance& operator=(Instance&&) = delete;

    VkInstance get_handle() const noexcept;
    bool is_extension_enabled(const char* extension) const noexcept;
    const std::vector<const char*>& get_enabled_extensions() const noexcept;
    const std::vector<const char*>& get_enabled_layers() const noexcept;

private:
    void get_supported_layers() noexcept;
    bool is_layer_supported(const char* layer_name) const noexcept;
    void get_supported_extensions() noexcept;
    bool is_extension_supported(const char* extension_name) const noexcept;

private:
    /** @brief Vulkan Instance handle */
    VkInstance instance_handle_{VK_NULL_HANDLE};

    /** @brief List of available and enabled Vulkan extensions */
    std::vector<const char*> enabled_extensions_;
    std::vector<VkExtensionProperties> available_instance_extensions_;

    /** @brief List of available and enabled Vulkan layers */
    std::vector<const char*> enabled_layers_;
    std::vector<VkLayerProperties> available_validation_layers_;

    /** @brief Vulkan validation callbacks and messenger for logging. */
    VkDebugUtilsMessengerEXT debug_utils_messenger_{VK_NULL_HANDLE};
};

}  // namespace Vulkan
}  // namespace String