#include <cstdlib>
#include <filesystem>
#include <string/application.hpp>
#include <iostream>
#include <string/core/logger.hpp>

namespace
{
// Resolve the resources root (which contains a `shaders/` directory):
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
}

int main()
{
    String::Application app;

    const std::filesystem::path resources_directory = resolve_resources_directory();
    STRING_LOG_INFO("Using resources directory: {}", resources_directory.string());

    String::ApplicationInfo app_info = {
        .application_name = "String Sandbox",
        .resources_directory = resources_directory,
    };

    try
    {
        STRING_LOG_DEBUG("Initializing application...");
        app.initialize(app_info);
        STRING_LOG_DEBUG("Running application...");
        app.run();
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}