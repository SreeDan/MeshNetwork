#include "mesh/utils/CommandUtils.h"

#include <charconv>

std::expected<std::pair<std::string, int>, std::string> parse_connect_args(
    const std::vector<std::string> &args) {
    if (args.size() != 2) {
        return std::unexpected("Usage: connect <host> <port>");
    }

    const std::string &host = args[0];
    if (host.empty()) {
        return std::unexpected("connect host must not be empty");
    }

    int port = 0;
    const std::string &raw_port = args[1];
    const char *begin = raw_port.data();
    const char *end = begin + raw_port.size();
    auto [ptr, ec] = std::from_chars(begin, end, port);
    if (ec != std::errc() || ptr != end) {
        return std::unexpected("connect port must be a number");
    }
    if (port < 1 || port > 65535) {
        return std::unexpected("connect port must be between 1 and 65535");
    }

    return std::make_pair(host, port);
}
