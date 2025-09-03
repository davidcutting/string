#include <cstdlib>

#include <string/core/logger.hpp>
#include <string/platform/platform.hpp>
#include <string/platform/window.hpp>

int main()
{
    STRING_LOG_INFO("Starting up the String Engine!");

    auto platform = String::Platform({
        .application_name = "String Test",
        .application_version = { 0, 0, 1 },
    });

    auto window = platform.create_window({
        .title = "String Test",
        .extent = { 800, 600 },
        .mode = String::WindowMode::Windowed,
        .resizable = true,
    }).value_or(nullptr);

    auto device = platform.create_device(window.get()).value_or(nullptr);

    while (!platform.poll_events())
    {
        // TODO
    }

    STRING_LOG_INFO("Shutting down the String Engine!");

    return EXIT_SUCCESS;
};