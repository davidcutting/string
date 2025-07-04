#include <string/platform.hpp>

namespace String
{

auto to_string(PlatformError error) -> std::string
{
    switch (error) {
        case PlatformError::WindowInitFailed:
            return "Failed to initialize window";
        case PlatformError::DeviceInitFailed:
            return "Failed to create device";
        default:
            return "Unknown platform error";
    }
}

}