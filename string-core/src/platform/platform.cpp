#include <string/platform/platform.hpp>

namespace String
{
enum class PlatformType : std::uint8_t
{
    WINDOWS,
    LINUX
};

#ifdef _WIN32
    constexpr PlatformType current_platform = PlatformType::WINDOWS;
#elif defined(__linux__)
    constexpr PlatformType current_platform = PlatformType::LINUX;
#else
    static_assert(false, "Unsupported platform!");
#endif

template<PlatformType P = current_platform>
class PlatformImpl;

using Platform = PlatformImpl<current_platform>;
}