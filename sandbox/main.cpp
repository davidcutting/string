#include <cstdlib>
#include <filesystem>
#include <iostream>

#include <string/application.hpp>
#include <string/core/logger.hpp>

#include <string/core/cvar.hpp>
#include <string/client/theme.hpp>
#include <string/debug/debug_cvars.hpp>
#include <string/render/render_cvars.hpp>
#include <string/ui/theme.hpp>

#include "debug_cvars.hpp"
#include "demo_scene.hpp"

namespace
{
// Resolve the resources root (which contains a `shaders/` directory, and at runtime an
// `assets/` one):
//   1. $STRING_RESOURCES_DIR if set (the Nix package wraps the binary to point here),
//   2. else $XDG_CONFIG_HOME/string, else $HOME/.config/string.
std::filesystem::path resolve_resources_directory()
{
    if (const char* env = std::getenv("STRING_RESOURCES_DIR"))
        return env;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"))
        return std::filesystem::path(xdg) / "string";
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home) / ".config" / "string";
    return std::filesystem::current_path();
}
}  // namespace

int main()
{
    String::Application app;

    // Register every library's CVars, THEN apply the STRING_* env bridge once — all before the
    // plan/passes are built, so they read already-overridden values at construction.
    //
    // The order is load-bearing: apply_env() only overrides CVars that are already registered, so
    // every library must have declared its own first. Each register_* call is pure registration
    // and the app owns the single bridge call, which is why a library can't get this wrong.
    string::render::register_render_cvars();
    string::debug::register_debug_cvars();
    sandbox::register_debug_cvars();
    string::core::CVarRegistry::instance().apply_env();

    // The kit owns the Theme type, the CLIENT owns the values, and the app is what installs one so
    // that both the game screens and the debug shell read the same palette without either library
    // depending on the other.
    string::ui::set_theme(string::client::theme());

    const std::filesystem::path resources_directory = resolve_resources_directory();
    STRING_LOG_INFO("Using resources directory: {}", resources_directory.string());

    String::ApplicationInfo app_info = {
        .application_name = "String Sandbox",
        .resources_directory = resources_directory,
    };

    try
    {
        STRING_LOG_DEBUG("Initializing application...");
        app.initialize(app_info, sandbox::build_demo_plan(resources_directory));
        STRING_LOG_DEBUG("Running application...");
        app.run();
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
