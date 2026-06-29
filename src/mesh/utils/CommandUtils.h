#pragma once

#include <expected>
#include <string>
#include <utility>
#include <vector>

std::expected<std::pair<std::string, int>, std::string> parse_connect_args(
    const std::vector<std::string> &args);
