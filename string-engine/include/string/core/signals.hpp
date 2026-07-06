#pragma once

#include <string/core/platform_detection.hpp>
#include <string/core/logger.hpp>
#include <atomic>

static std::atomic<bool> g_signal_quit = false;

void init_signal_handling();

#if defined(STRING_PLATFORM_LINUX)
#include <csignal>

inline void handle_sigint(int /*signum*/)
{
    STRING_LOG_CRITICAL("Signal recieved, attempting shutdown...");
    g_signal_quit.store(true, std::memory_order_relaxed);
}

inline void init_signal_handling()
{
    std::signal(SIGINT, handle_sigint);
}

#elif defined(STRING_PLATFORM_WINDOWS)

#include <windows.h>

inline BOOL WINAPI ConsoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT)
    {
        g_signal_quit.store(true, std::memory_order_relaxed);
    }
    return TRUE;
}

inline void init_signal_handling()
{
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
}

#endif