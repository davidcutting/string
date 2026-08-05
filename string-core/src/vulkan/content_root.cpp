#include <string/vulkan/content_root.hpp>

#include <cstdlib>
#include <fstream>

#include <string/core/cvar.hpp>
#include <string/core/logger.hpp>
#include <string/core/user_dirs.hpp>

namespace String
{

string::core::CVar<std::string>& cv_content_root()
{
    static string::core::CVar<std::string> v{
        "content.root", "",
        "folder to discover scenes in. Set at runtime to persist it (takes effect next launch); "
        "STRING_CONTENT_ROOT sets it for this run only."};
    return v;
}

namespace
{
std::filesystem::path& default_root()
{
    static std::filesystem::path d;
    return d;
}
}  // namespace

void ContentRoot::set_default(std::filesystem::path dir)
{
    default_root() = std::move(dir);
}

std::filesystem::path ContentRoot::settings_path()
{
    return string::core::user_config_dir() / "content_root.txt";
}

std::filesystem::path ContentRoot::resolve()
{
    // Both env spellings are read DIRECTLY rather than through the cvar's env alias: resolution runs
    // during scene registration, which is not ordered against the registry's env sweep, so relying
    // on the alias would work or not depending on static init order.
    for (const char* name : { "STRING_CONTENT_DIR", "STRING_CONTENT_ROOT" })
    {
        if (const char* env = std::getenv(name); env != nullptr && *env != '\0')
        {
            return std::filesystem::path(env);
        }
    }

    // The cvar, for a value set on the command line. Ranks above the persisted setting so a
    // one-run override does not have to be un-set afterwards.
    if (const std::string from_cvar = cv_content_root().get(); !from_cvar.empty())
    {
        return std::filesystem::path(from_cvar);
    }

    std::error_code ec;
    const std::filesystem::path settings = settings_path();
    if (std::filesystem::exists(settings, ec))
    {
        std::ifstream in(settings);
        std::string line;
        if (std::getline(in, line) && !line.empty())
        {
            return std::filesystem::path(line);
        }
    }

    return default_root().empty() ? std::filesystem::current_path() / "assets" : default_root();
}

const std::filesystem::path& ContentRoot::get()
{
    static const std::filesystem::path root = [] {
        const std::filesystem::path r = resolve();
        std::error_code ec;
        if (std::filesystem::is_directory(r, ec))
        {
            STRING_LOG_INFO("[content] root: {}", r.string());
        }
        else
        {
            // Not an error. A clone with no content still starts; the scene list is just empty.
            STRING_LOG_INFO("[content] root {} does not exist — no content scenes. "
                            "Set one with `content.root <dir>`.", r.string());
        }
        return r;
    }();
    return root;
}

bool ContentRoot::set(const std::filesystem::path& dir, std::string& error_out)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
    {
        error_out = "not a directory: " + dir.string();
        return false;
    }

    const std::filesystem::path settings = settings_path();
    std::filesystem::create_directories(settings.parent_path(), ec);
    std::ofstream out(settings, std::ios::trunc);
    if (!out)
    {
        error_out = "could not write " + settings.string();
        return false;
    }
    out << std::filesystem::absolute(dir, ec).string() << '\n';

    // Deliberately NOT applied to the live root: get() is resolved once and scenes are already
    // registered against it. Telling the user to restart is honest; silently half-applying is not.
    STRING_LOG_INFO("[content] root saved to {} — restart to use it", settings.string());
    return true;
}

}  // namespace String
