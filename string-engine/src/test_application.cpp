#include <filesystem>
#include <string/application.hpp>
#include <iostream>
#include <string/core/logger.hpp>

int main()
{
    String::Application app;

    std::filesystem::path test_path = ".";

    String::ApplicationInfo app_info = {
        .application_name = "String Sandbox",
        .resources_directory = "/home/dcutting/.config/string/",
        // .resources_directory = test_path,
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