#pragma once

#include <format>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <source_location>
#include <string>
#include <string_view>
#include <mutex>
#include <vector>
#include <string/core/platform_detection.hpp>

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

// A small thread-safe ring buffer that mirrors the most recent log lines so an in-engine console
// (brief 06) can render a live log tail without owning the logger. Every Logger::log_impl call
// pushes its formatted line (with severity) here in addition to stdout; the console snapshots the
// tail each frame. Fixed capacity, lock-guarded, no allocation on the hot path beyond the line
// string itself. Process-global (the logger is a singleton, so its sink is too).
class LogRingBuffer
{
public:
    struct Line
    {
        LogLevel level;
        std::string text;   // "[HH:MM:SS] [LEVEL] message"
    };

    static LogRingBuffer& instance()
    {
        static LogRingBuffer buf;
        return buf;
    }

    void push(LogLevel level, std::string text)
    {
        std::lock_guard lock(mutex_);
        if (lines_.size() < capacity_)
        {
            lines_.push_back(Line{ level, std::move(text) });
        }
        else
        {
            lines_[head_] = Line{ level, std::move(text) };
            head_ = (head_ + 1) % capacity_;
        }
        ++total_;
    }

    // Snapshot the last `count` lines in chronological order (oldest first). Cheap enough for a
    // per-frame console (small N, short strings); copies under the lock so the caller is decoupled.
    std::vector<Line> tail(std::size_t count) const
    {
        std::lock_guard lock(mutex_);
        const std::size_t n = std::min(count, lines_.size());
        std::vector<Line> out;
        out.reserve(n);
        const std::size_t start = lines_.size() < capacity_ ? lines_.size() - n
                                                            : (head_ + (capacity_ - n)) % capacity_;
        for (std::size_t i = 0; i < n; ++i)
        {
            out.push_back(lines_[(start + i) % lines_.size()]);
        }
        return out;
    }

    // Monotonic count of lines ever pushed — lets the console detect new lines to auto-scroll.
    std::size_t total() const
    {
        std::lock_guard lock(mutex_);
        return total_;
    }

private:
    LogRingBuffer() = default;
    static constexpr std::size_t capacity_ = 512;
    mutable std::mutex mutex_;
    std::vector<Line> lines_;
    std::size_t head_ = 0;    // next write slot once full
    std::size_t total_ = 0;
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
                  [[maybe_unused]] const std::source_location& loc,
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

        // NOT std::println: its unicode-terminal path needs std::__open_terminal /
        // std::__write_to_terminal, which mingw's libstdc++ does not provide, so every binary fails
        // to link when cross-compiling to Windows. The message is already formatted here, so
        // println was only doing the write. Cost is losing println's UTF-16 console handling on
        // Windows; log lines are ASCII.
        std::fputs(std::format("[{}] [{}] {}\n", timestamp, level_str, message).c_str(), stdout);
        // Explicit flush: stdout is block-buffered when redirected to a file or pipe, which is how
        // every capture/gate script runs the engine. Without this the last lines before a crash —
        // exactly the ones worth having — are lost in the unflushed buffer.
        std::fflush(stdout);

        // Mirror into the in-engine console ring (brief 06). Timestamp trimmed to HH:MM:SS to keep
        // the console line short; severity is carried structurally for colouring.
        std::string_view ts = timestamp;
        if (ts.size() > 11) ts = ts.substr(11);  // drop "YYYY-MM-DD "
        LogRingBuffer::instance().push(
            level, std::format("[{}] [{}] {}", ts, level_str, message));
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
