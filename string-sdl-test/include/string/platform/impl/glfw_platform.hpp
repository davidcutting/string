#pragma once

#include <string/platform/platform.hpp>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <print>

namespace String::Platform
{

class Platform::Impl
{
public:
    GLFWwindow* window;

    explicit Impl();
    ~Impl();

    bool poll_events();
    bool attempt_wayland_init() const;
};

}