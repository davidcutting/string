#include <string/platform/impl/sdl_platform.hpp>

namespace String::Platform
{

Platform::Impl::Impl()
{
    std::println("Initializing SDL...");

    bool initialized = false;
    for (const auto& wsi_lib : get_all_wsi_lib())
    {
        if (attempt_sdl_init(wsi_lib))
        {
            initialized = true;
            break;
        }
    }

    if (!initialized)
    {
        throw std::runtime_error("Failed to initialize platform - " + std::string(SDL_GetError()));
    }

    std::println("SDL initialized successfully");
}

Platform::Impl::~Impl()
{
    std::println("Shutting down SDL...");
    SDL_Quit();
}

auto Platform::Impl::get_all_wsi_lib() -> std::vector<std::string>
{
    std::vector<std::string> wsi_libs;
    int num_drivers = SDL_GetNumVideoDrivers();
    for (int i = 0; i < num_drivers; ++i)
    {
        const auto video_driver = SDL_GetVideoDriver(i);
        wsi_libs.push_back(video_driver);
        std::println("Found video driver: {}", video_driver);
    }
    return wsi_libs;
}

bool Platform::Impl::attempt_sdl_init(const std::string& wsi_lib)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, wsi_lib.c_str());
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::println("Failed to initialize with {}", wsi_lib);
        return false;
    }
    std::println("Initialized with {}", wsi_lib);
    return true;
}

bool Platform::Impl::poll_events()
{
    bool should_quit = false;
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_EVENT_QUIT)
            should_quit = true;
    }

    return should_quit;
}

}
