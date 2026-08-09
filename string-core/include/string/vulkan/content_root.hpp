#pragma once

#include <filesystem>
#include <string>

namespace string::core { template <typename T> class CVar; }

namespace string
{

// `content.root` — the console/env handle on the folder below. Declared here so both the resolver
// and the runtime "user typed a new value" poll read the same CVar instance.
string::core::CVar<std::string>& cv_content_root();

// The folder the user keeps their content in: scenes are discovered here and imports are written
// here. Deliberately NOT a path inside the engine repo — the artist keeps their work where they
// keep their work, and it survives re-cloning the engine.
//
// Resolution order, first wins:
//   1. STRING_CONTENT_DIR         (env; also how headless gating points at a fixture)
//   2. the persisted setting      (user_config_dir()/content_root.txt, written by ContentRoot::set)
//   3. `<cwd>/assets`             (the historical layout, so an existing checkout keeps working)
//
// An empty or missing folder is NORMAL, not an error: a clone with no content must still start.
class ContentRoot
{
public:
    // The fallback used when nothing else specifies a root. The app sets it to its resources
    // directory's `assets/`, which is where an existing checkout already keeps content — the process
    // CWD is not that place and defaulting to it silently found nothing. Must be called before the
    // first get(), which memoises.
    static void set_default(std::filesystem::path dir);

    // Resolve (once) and return the active content root. May not exist on disk.
    static const std::filesystem::path& get();

    // Point the engine at `dir` and persist it for next launch. Returns false (and changes nothing)
    // if the path is not an existing directory, so a typo cannot strand the user with a dead root.
    static bool set(const std::filesystem::path& dir, std::string& error_out);

    // Where the setting is persisted — surfaced so the UI/console can tell the user.
    static std::filesystem::path settings_path();

private:
    static std::filesystem::path resolve();
};

}  // namespace string
