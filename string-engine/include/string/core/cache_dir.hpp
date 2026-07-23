#pragma once

#include <cstdlib>
#include <filesystem>

namespace string::core
{

// Per-user persistent cache root for content-hash caches (shaders, meshlets). XDG_CACHE_HOME,
// else ~/.cache, else the OS temp dir (which is wiped on reboot — last resort only, a cold
// rebuild there costs the user a full re-meshletize/recompile on next launch).
inline std::filesystem::path user_cache_dir(const std::filesystem::path& subdir)
{
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "string" / subdir;
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".cache" / "string" / subdir;
    return std::filesystem::temp_directory_path() / ("string-" + subdir.string());
}

}  // namespace string::core
