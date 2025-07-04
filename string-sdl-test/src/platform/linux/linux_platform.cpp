#include <string/platform.hpp>
#include <string/logger.hpp>

namespace String
{

struct Platform::Impl
{

};

Platform::Platform()
{

}

Platform::~Platform()
{

}

auto Platform::create_window(const WindowInfo& info) -> std::expected<std::unique_ptr<Window>, PlatformError>
{

}

auto Platform::create_device(const DeviceInfo& info) -> std::expected<std::unique_ptr<Device>, PlatformError>
{

}

bool Platform::poll_events()
{

}

}