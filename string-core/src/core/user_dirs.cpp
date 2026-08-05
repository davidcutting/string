#include <string/core/user_dirs.hpp>

#include <string/core/platform_detection.hpp>

#ifdef STRING_PLATFORM_WINDOWS
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace string::core
{
namespace
{

// A non-empty environment variable as a path, else nullopt-ish (an empty path).
std::filesystem::path env_path(const char* name)
{
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::filesystem::path(v) : std::filesystem::path{};
}

std::filesystem::path with_sub(const std::filesystem::path& root, const std::filesystem::path& sub)
{
    return sub.empty() ? root : root / sub;
}

}  // namespace

std::filesystem::path executable_dir()
{
    std::error_code ec;
#ifdef STRING_PLATFORM_WINDOWS
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return {};
    }
    return std::filesystem::path(buf, buf + n).parent_path();
#else
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : exe.parent_path();
#endif
}

bool portable_mode()
{
    const std::filesystem::path dir = executable_dir();
    if (dir.empty())
    {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(dir / "portable.txt", ec);
}

std::filesystem::path user_config_dir(const std::filesystem::path& subdir)
{
    if (const std::filesystem::path override_dir = env_path("STRING_CONFIG_DIR"); !override_dir.empty())
    {
        return with_sub(override_dir, subdir);
    }
    if (portable_mode())
    {
        return with_sub(executable_dir() / "config", subdir);
    }
#ifdef STRING_PLATFORM_WINDOWS
    // Roaming is correct here: config is tiny and users expect it to follow them.
    if (const std::filesystem::path appdata = env_path("APPDATA"); !appdata.empty())
    {
        return with_sub(appdata / "string", subdir);
    }
#else
    if (const std::filesystem::path xdg = env_path("XDG_CONFIG_HOME"); !xdg.empty())
    {
        return with_sub(xdg / "string", subdir);
    }
    if (const std::filesystem::path home = env_path("HOME"); !home.empty())
    {
        return with_sub(home / ".config" / "string", subdir);
    }
#endif
    return with_sub(std::filesystem::current_path() / ".string", subdir);
}

std::filesystem::path user_cache_dir(const std::filesystem::path& subdir)
{
    if (const std::filesystem::path override_dir = env_path("STRING_CACHE_DIR"); !override_dir.empty())
    {
        return with_sub(override_dir, subdir);
    }
    if (portable_mode())
    {
        return with_sub(executable_dir() / "cache", subdir);
    }
#ifdef STRING_PLATFORM_WINDOWS
    // LOCAL, not Roaming — this holds gigabytes of shader/meshlet caches, and a roaming profile
    // would sync all of it to a server at login/logout on a domain-joined machine.
    if (const std::filesystem::path local = env_path("LOCALAPPDATA"); !local.empty())
    {
        return with_sub(local / "string" / "cache", subdir);
    }
#else
    if (const std::filesystem::path xdg = env_path("XDG_CACHE_HOME"); !xdg.empty())
    {
        return with_sub(xdg / "string", subdir);
    }
    if (const std::filesystem::path home = env_path("HOME"); !home.empty())
    {
        return with_sub(home / ".cache" / "string", subdir);
    }
#endif
    // Wiped on reboot — a cold rebuild here costs a full re-meshletize/recompile next launch.
    return std::filesystem::temp_directory_path() / ("string-" + subdir.string());
}

}  // namespace string::core
