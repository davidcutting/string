#include <string/core/logger.hpp>

#include <spdlog/fmt/ostr.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace String
{

void Logger::initialize() noexcept {
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

const std::shared_ptr<spdlog::logger>& Logger::get_logger() noexcept {
    STRING_ASSERT(logger_inst_ != nullptr);
    return logger_inst_;
}

}