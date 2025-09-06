#pragma once

#include <cstdint>

namespace String
{

enum class PlatformType : std::uint8_t
{
    WINDOWS,
    LINUX
};

#ifdef _WIN32
    static constexpr PlatformType current_platform = PlatformType::WINDOWS;
    #define STRING_PLATFORM_WINDOWS
#elif defined(__linux__)
    static constexpr PlatformType current_platform = PlatformType::LINUX;
    #define VK_USE_PLATFORM_WAYLAND_KHR
    #define STRING_PLATFORM_LINUX
#else
    static_assert(false, "Unsupported platform!");
#endif

enum class ReleaseType : std::uint8_t
{
    DEBUG,
    RELEASE
};

#ifdef DEBUG
    static constexpr ReleaseType current_release = ReleaseType::DEBUG;
    #define STRING_DEBUG
#elifdef NDEBUG
    static constexpr ReleaseType current_release = ReleaseType::RELEASE;
    #define STRING_RELEASE
#else
    static constexpr ReleaseType current_release = ReleaseType::DEBUG;
    #define STRING_DEBUG
#endif

}