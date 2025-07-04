#include <expected>
#include <string/platform.hpp>
#include <string/logger.hpp>
#include <SDL3/SDL.h>
#include <vector>

namespace String
{

struct Platform::Impl
{

};

inline auto get_all_wsi_lib() -> std::vector<std::string>
{
    std::vector<std::string> wsi_libs;
    int num_drivers = SDL_GetNumVideoDrivers();
    for (int i = 0; i < num_drivers; ++i)
    {
        const auto video_driver = SDL_GetVideoDriver(i);
        wsi_libs.push_back(video_driver);
        
        STRING_LOG_DEBUG("Found video driver: {}", video_driver);
    }
    return wsi_libs;
}

inline bool attempt_sdl_init(const std::string& wsi_lib)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, wsi_lib.c_str());
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        STRING_LOG_DEBUG("Failed to initialize with {}", wsi_lib);
        return false;
    }
    STRING_LOG_DEBUG("Initialized with {}", wsi_lib);
    return true;
}

Platform::Platform()
{
    STRING_LOG_INFO("Initializing SDL...");

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

    STRING_LOG_INFO("SDL initialized successfully");
}

Platform::~Platform()
{
    STRING_LOG_INFO("Shutting down SDL...");
    SDL_Quit();
}

auto Platform::create_window(const WindowInfo& info) -> std::expected<std::unique_ptr<Window>, PlatformError>
{
    try
    {
        return std::move(std::make_unique<Window>(info));
    }
    catch (const std::exception& e)
    {
        STRING_LOG_ERROR("Failed to create window: ", e.what());
        return std::unexpected<PlatformError>(PlatformError::WindowInitFailed);
    }
}

auto Platform::create_device(const DeviceInfo& info) -> std::expected<std::unique_ptr<Device>, PlatformError>
{
    // TODO impl
}

bool Platform::poll_events()
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
