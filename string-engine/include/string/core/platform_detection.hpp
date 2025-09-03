#pragma once

#include <cstdint>

namespace string
{

enum class PlatformType : std::uint8_t
{
    WINDOWS,
    LINUX
};

#ifdef _WIN32
    constexpr PlatformType current_platform = PlatformType::WINDOWS;
    #define STRING_PLATFORM_WINDOWS
#elif defined(__linux__)
    constexpr PlatformType current_platform = PlatformType::LINUX;
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
    constexpr ReleaseType current_release = ReleaseType::DEBUG;
    #define STRING_DEBUG
#else
constexpr ReleaseType current_release = ReleaseType::RELEASE;
    #define STRING_RELEASE
#endif

}
