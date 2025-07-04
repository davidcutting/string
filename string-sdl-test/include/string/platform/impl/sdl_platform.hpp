#pragma once

#include <string/platform/platform.hpp>
#include <SDL3/SDL.h>
#include <vector>
#include <print>

namespace String::Platform
{

class Platform::Impl
{
public:
    explicit Impl();
    ~Impl();
    auto get_all_wsi_lib() -> std::vector<std::string>;
    bool attempt_sdl_init(const std::string& wsi_lib);
    bool poll_events();
};

}
