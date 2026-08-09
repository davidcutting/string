#pragma once

#include <cstdlib>
#include <filesystem>

namespace string::core
{

// Per-user CONFIG and CACHE roots, plus portable mode.
//
// Config and cache are deliberately separate homes because they are different kinds of data:
//
//   config  bytes       settings that must survive a cache wipe (the content root)
//   cache   gigabytes   regenerable content-hash caches (shaders, meshlets)
//
// On Windows that distinction is load-bearing, not cosmetic: %APPDATA% is ROAMING and syncs to a
// server at login/logout on domain-joined machines, so a multi-gigabyte cache there is a real
// failure. Large regenerable data belongs in %LOCALAPPDATA%.
//
// PORTABLE MODE: a `portable.txt` beside the executable puts both roots under the install directory
// and writes nothing outside it — for a USB stick or a checkout the user wants to delete in one go.
// STRING_CONFIG_DIR / STRING_CACHE_DIR override either explicitly and win over everything.

// Directory containing the running executable, or empty if it cannot be determined.
std::filesystem::path executable_dir();

// True when a `portable.txt` marker sits beside the executable.
bool portable_mode();

// Per-user config root. Order: STRING_CONFIG_DIR, portable ./config, then the platform location
// (%APPDATA% on Windows, XDG_CONFIG_HOME / ~/.config elsewhere), then the cwd as a last resort.
std::filesystem::path user_config_dir(const std::filesystem::path& subdir = {});

// Per-user cache root. Order: STRING_CACHE_DIR, portable ./cache, then the platform location
// (%LOCALAPPDATA% on Windows — Local, NOT Roaming; XDG_CACHE_HOME / ~/.cache elsewhere), then the
// OS temp dir. Temp is a genuine last resort: it is wiped on reboot, so a cold rebuild there costs
// the user a full re-meshletize/recompile on next launch.
std::filesystem::path user_cache_dir(const std::filesystem::path& subdir = {});

}  // namespace string::core
