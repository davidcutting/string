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

#include <string/core/debug.hpp>
#include <string/core/version.hpp>

#include <bits/stdc++.h>

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

namespace String {
namespace Vulkan {

class Instance {
public:
    Instance(const std::string& application_name, const Version& application_version);
    ~Instance();

    VkInstance get_handle() const noexcept;
private:
    VkInstance instance_handle_ = VK_NULL_HANDLE;
    uint32_t num_extensions_available = 0;
};

}  // namespace Vulkan
}  // namespace String