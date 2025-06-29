#pragma once

#include <experimental/propagate_const>
#include <memory>
#include <string/platform/window.hpp>

#include <cstdint>
#include <string>
#include <vulkan/vulkan.h>

namespace String
{

enum class DeviceType : std::uint8_t
{
    DISCRETE,
    INTEGRATED,
    CPU,
    VIRTUAL,
    ANY
};

struct DeviceInfo
{
};

class Device
{
    class Impl;
    using impl_t = std::experimental::propagate_const<std::unique_ptr<Impl>>;
    impl_t impl_;
public:
    // Disable copy and move
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;
    
    // Device info
    std::string get_device_name() const;
    DeviceType get_device_type() const;
    uint64_t get_memory_size() const;

    auto create_buffer(const BufferInfo& buffer_info) -> Buffer;
    auto create_image(const ImageInfo& image_info) -> Image;

private:
    friend Platform;
    Device(const DeviceInfo& desc);
};

}
