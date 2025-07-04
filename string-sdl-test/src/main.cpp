#include <cstdlib>
#include <print>
#include <entt/entity/registry.hpp>
#include <string/platform/platform.hpp>
#include <string/platform/window.hpp>
#include <thread>
#include <chrono>

auto main() -> int
{
    auto platform = String::Platform::Platform();

    auto window = platform.create_window({
        .title = "String Test",
        .extent = {800, 600},
        .mode = String::Platform::WindowMode::Windowed,
        .resizable = false,
    });

    while (platform.poll_events())
    {
        // Literally just burn time
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return EXIT_SUCCESS;
};
