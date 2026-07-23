#include <cstdlib>
#include <filesystem>
#include <iostream>

#include <string/application.hpp>
#include <string/core/logger.hpp>

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

    // Register the sandbox debug CVars and apply the STRING_* env bridge (legacy aliases honoured)
    // before the plan/passes are built, so the passes read already-overridden values at construction.
    sandbox::register_debug_cvars();

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
