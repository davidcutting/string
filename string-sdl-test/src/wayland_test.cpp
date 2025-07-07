#include <cstring>
#include <span>
#include <thread>

#include <string/platform/wayland/client.hpp>
#include <string/platform/wayland/window.hpp>
#include <string/platform/wayland/presenter.hpp>
#include <string/logger.hpp>

int main(int argc, char *argv[])
{
    STRING_LOG_INFO("Starting up!");

    auto client = wl::client();
    auto* window = client.create_window();
    auto* presenter = client.create_presenter(window);

    STRING_LOG_INFO("Starting event loop...");

    while (!window->should_close())
    {
        const auto frame = presenter->begin_frame();

        // make it somethin?
        std::memset(frame.data(), 0xFF, frame.size());

        presenter->present();

        client.poll_events();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    STRING_LOG_INFO("Shutting down...");

    delete presenter;
    client.delete_window(window);

    return EXIT_SUCCESS;
}
