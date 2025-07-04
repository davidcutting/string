#pragma once

#include <string/platform/window.hpp>
#include <string/platform/device.hpp>
#include <expected>
#include <string>
#include <memory>

namespace String::Platform
{

enum class PlatformError
{
    WindowInitFailed,
    DeviceInitFailed,
};

auto to_string(PlatformError error) -> std::string;

class Platform
{
    class Impl;
    std::unique_ptr<Impl> pimpl_;
public:
    Platform();
    ~Platform();

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;
    Platform(Platform&&) = delete;
    Platform& operator=(Platform&&) = delete;

    auto create_window(const WindowInfo& info) -> std::expected<std::unique_ptr<Window>, PlatformError>;
    auto create_device(const DeviceInfo& info) -> std::expected<std::unique_ptr<Device>, PlatformError>;

    bool poll_events();
};

}