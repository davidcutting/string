#pragma once

// Compatibility header: `user_cache_dir` moved to user_dirs.hpp, which gained the config/cache
// split, real Windows branches (%LOCALAPPDATA% for cache — Local, NOT Roaming) and portable mode.
// Existing includers keep working; new code should include <string/core/user_dirs.hpp> directly.

#include <string/core/user_dirs.hpp>
