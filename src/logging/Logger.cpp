#include "Logger.h"

#include <iostream>

#include "spdlog/spdlog.h"
#include "spdlog/sinks/rotating_file_sink.h"


// std::string safe_dump(const json &context) {
//     return context.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
// }

void Log::init(const std::string &node_id, const std::string &filename, bool debug_mode, int max_size, int max_files) {
    try {
        auto logger = spdlog::rotating_logger_mt("app_logger", filename, max_size, max_files);
        logger->set_pattern("%Y-%m-%d %H:%M:%S [" + node_id + "] [%l] %v");

        if (debug_mode) {
            logger->set_level(spdlog::level::debug);
        } else {
            logger->set_level(spdlog::level::info);
        }

        spdlog::set_default_logger(logger);
        spdlog::flush_every(std::chrono::milliseconds(100));
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
