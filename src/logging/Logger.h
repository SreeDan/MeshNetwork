#pragma once

#include "nlohmann/json.hpp"

using json = nlohmann::json;

constexpr int DEFAULT_LOG_FILE_SIZE = 1048576 * 5;
constexpr int DEFAULT_LOG_MAX_FILES = 3;

class Log {
public:
    static void init(
        const std::string &filename,
        int max_size = DEFAULT_LOG_FILE_SIZE,
        int max_files = DEFAULT_LOG_MAX_FILES
    );

    static void info(const std::string &component, const json &context, const std::string &message);

    static void warn(const std::string &component, const json &context, const std::string &message);

    static void error(const std::string &component, const json &context, const std::string &message);
    static void debug(const std::string &component, const json &context, const std::string &message);
};
