#include "Logger.h"

#include <iostream>

#include "spdlog/spdlog.h"
#include "spdlog/sinks/rotating_file_sink.h"


void Log::init(const std::string &filename, int max_size, int max_files) {
    try {
        auto logger = spdlog::rotating_logger_mt("app_logger", filename, max_size, max_files);
        logger->set_pattern("%Y-%m-%d %H:%M:%S [%l] %v");

        spdlog::set_default_logger(logger);
        spdlog::flush_on(spdlog::level::info);
    } catch (const spdlog::spdlog_ex &ex) {
        std::cerr << "log initialization failed" << ex.what() << std::endl;
    }
}

void Log::info(const std::string &component, const json &context, const std::string &message) {
    spdlog::info("[{}] {} | {}", component, message, context.dump());
}

void Log::warn(const std::string &component, const json &context, const std::string &message) {
    spdlog::warn("[{}] {} | {}", component, message, context.dump());
}

void Log::error(const std::string &component, const json &context, const std::string &message) {
    spdlog::error("[{}] {} | {}", component, message, context.dump());
}

void Log::debug(const std::string &component, const json &context, const std::string &message) {
    spdlog::debug("[{}] {} | {}", component, message, context.dump());
}