#pragma once

#include <print>
#include <format>
#include <chrono>
#include <source_location>
#include <string_view>
#include <mutex>

namespace String
{

enum class LogLevel : int
{
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARN = 3,
    ERROR = 4,
    CRITICAL = 5
};

class Logger
{
private:
    mutable std::mutex mutex_;
    
    Logger() = default;
    ~Logger() = default;

public:
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static Logger& get_logger()
    {
        static Logger instance;
        return instance;
    }

    // Main logging function template - std::print compatible interface
    template<typename... Args>
    void log(LogLevel level, 
             std::format_string<Args...> fmt, 
             Args&&... args) const
    {
        log_impl(level, fmt, std::source_location::current(), std::forward<Args>(args)...);
    }

private:
    template<typename... Args>
    void log_impl(LogLevel level, 
                  std::format_string<Args...> fmt, 
                  const std::source_location& loc,
                  Args&&... args) const
    {
        std::lock_guard lock(mutex_);

        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);

        auto timestamp = std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}",
                                   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                                   tm.tm_hour, tm.tm_min, tm.tm_sec);

        std::string_view level_str = get_level_string(level);

        std::string message;
        if constexpr (sizeof...(args) > 0) {
            message = std::format(fmt, std::forward<Args>(args)...);
        } else {
            message = fmt.get();
        }

        std::println("[{}] [{}] {}", 
                    timestamp, level_str, message);
    }

public:

    template<typename... Args>
    void trace(std::format_string<Args...> fmt, Args&&... args) const
    {
        log_impl(LogLevel::TRACE, fmt, std::source_location::current(), std::forward<Args>(args)...);
    }

    template<typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args) const
    {
        log_impl(LogLevel::DEBUG, fmt, std::source_location::current(), std::forward<Args>(args)...);
    }

    template<typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) const
    {
        log_impl(LogLevel::INFO, fmt, std::source_location::current(), std::forward<Args>(args)...);
    }

    template<typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) const
    {
        log_impl(LogLevel::WARN, fmt, std::source_location::current(), std::forward<Args>(args)...);
    }

    template<typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) const
    {
        log_impl(LogLevel::ERROR, fmt, std::source_location::current(), std::forward<Args>(args)...);
    }

    template<typename... Args>
    void critical(std::format_string<Args...> fmt, Args&&... args) const
    {
        log_impl(LogLevel::CRITICAL, fmt, std::source_location::current(), std::forward<Args>(args)...);
    }

private:
    static constexpr std::string_view get_level_string(LogLevel level) noexcept
    {
        switch (level) {
            case LogLevel::TRACE:    return "TRACE";
            case LogLevel::DEBUG:    return "DEBUG";
            case LogLevel::INFO:     return "INFO";
            case LogLevel::WARN:     return "WARN";
            case LogLevel::ERROR:    return "ERROR";
            case LogLevel::CRITICAL: return "CRITICAL";
            default:                 return "UNKNOWN";
        }
    }
};

#ifndef STRING_RELEASE
#define STRING_LOG_TRACE(fmt, ...) ::String::Logger::get_logger().trace(fmt __VA_OPT__(,) __VA_ARGS__)
#define STRING_LOG_DEBUG(fmt, ...) ::String::Logger::get_logger().debug(fmt __VA_OPT__(,) __VA_ARGS__)
#define STRING_LOG_INFO(fmt, ...)  ::String::Logger::get_logger().info(fmt __VA_OPT__(,) __VA_ARGS__)
#define STRING_LOG_WARN(fmt, ...)  ::String::Logger::get_logger().warn(fmt __VA_OPT__(,) __VA_ARGS__)
#else
#define STRING_LOG_TRACE(fmt, ...) ((void)0)
#define STRING_LOG_DEBUG(fmt, ...) ((void)0)
#define STRING_LOG_INFO(fmt, ...)  ((void)0)
#define STRING_LOG_WARN(fmt, ...)  ((void)0)
#endif  // STRING_RELEASE

#define STRING_LOG_ERROR(fmt, ...)    ::String::Logger::get_logger().error(fmt __VA_OPT__(,) __VA_ARGS__)
#define STRING_LOG_CRITICAL(fmt, ...) ::String::Logger::get_logger().critical(fmt __VA_OPT__(,) __VA_ARGS__)

}
