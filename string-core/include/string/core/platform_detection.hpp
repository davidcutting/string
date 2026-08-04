#pragma once

#include <cstdint>

namespace String
{

enum class PlatformType : std::uint8_t
{
    WINDOWS,
    LINUX
};

// meson also defines STRING_PLATFORM_<OS> on the command line (see the `system ==` branch in
// string-core/meson.build), so guard the definitions here — an unguarded #define is a redefinition
// warning on every translation unit. Only shows up when the two agree that we are on that platform,
// i.e. never on the Linux-native build; found by cross-compiling to Windows.
#ifdef _WIN32
    static constexpr PlatformType current_platform = PlatformType::WINDOWS;
    #ifndef STRING_PLATFORM_WINDOWS
        #define STRING_PLATFORM_WINDOWS
    #endif
#elif defined(__linux__)
    static constexpr PlatformType current_platform = PlatformType::LINUX;
    #define VK_USE_PLATFORM_WAYLAND_KHR
    #ifndef STRING_PLATFORM_LINUX
        #define STRING_PLATFORM_LINUX
    #endif
#else
    static_assert(false, "Unsupported platform!");
#endif

enum class ReleaseType : std::uint8_t
{
    DEBUG,
    RELEASE
};

#ifdef NDEBUG
    static constexpr ReleaseType current_release = ReleaseType::RELEASE;
    #ifndef STRING_RELEASE
    #define STRING_RELEASE
    #endif
#else
    static constexpr ReleaseType current_release = ReleaseType::DEBUG;
    #ifndef STRING_DEBUG
    #define STRING_DEBUG
    #endif
#endif

}