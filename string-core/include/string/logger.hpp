// MIT License
//
// Copyright (c) 2022 David Cutting
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <bits/stdc++.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#ifndef STRING_RELEASE
#define STRING_STATIC_ASSERT(expression, message) static_assert(expression, message)
#define STRING_ASSERT(...) assert(__VA_ARGS__)
#else
#define STRING_STATIC_ASSERT(expression, message)
#define STRING_ASSERT(...)
#endif

namespace String {

class Logger {
public:
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    static void initialize() noexcept;
    static const std::shared_ptr<spdlog::logger>& get_logger() noexcept;

private:
    Logger() = default;

    inline static std::shared_ptr<spdlog::logger> logger_inst_;
};

inline void Logger::initialize() noexcept {
    std::vector<spdlog::sink_ptr> log_sinks;
    log_sinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    log_sinks.emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("string_runtime.log", true));

    log_sinks[0]->set_pattern("%^[%T] %n: %v%$");
    log_sinks[1]->set_pattern("[%T] [%l] %n: %v");

    logger_inst_ = std::make_shared<spdlog::logger>("String", std::begin(log_sinks), std::end(log_sinks));
    spdlog::register_logger(logger_inst_);
    logger_inst_->set_level(spdlog::level::trace);
    logger_inst_->flush_on(spdlog::level::trace);
}
inline const std::shared_ptr<spdlog::logger>& Logger::get_logger() noexcept {
    STRING_ASSERT(logger_inst_ != nullptr);
    return logger_inst_;
}

}  // namespace String

#ifndef STRING_RELEASE
#define STRING_LOG_TRACE(...) ::String::Logger::get_logger()->trace(__VA_ARGS__)
#define STRING_LOG_INFO(...) ::String::Logger::get_logger()->info(__VA_ARGS__)
#define STRING_LOG_WARN(...) ::String::Logger::get_logger()->warn(__VA_ARGS__)
#else
#define STRING_LOG_TRACE(...)
#define STRING_LOG_INFO(...)
#define STRING_LOG_WARN(...)
#endif  // STRING_RELEASE
#define STRING_LOG_ERROR(...) ::String::Logger::get_logger()->error(__VA_ARGS__)
#define STRING_LOG_CRTICAL(...) ::String::Logger::get_logger()->critical(__VA_ARGS__)
