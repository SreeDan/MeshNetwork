#include <catch2/catch_test_macros.hpp>

#include "mesh/utils/CommandUtils.h"

TEST_CASE("parse_connect_args accepts exactly host and numeric port", "[cli]") {
    auto parsed = parse_connect_args({"127.0.0.1", "8082"});

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->first == "127.0.0.1");
    REQUIRE(parsed->second == 8082);
}

TEST_CASE("parse_connect_args rejects malformed connect commands", "[cli]") {
    REQUIRE_FALSE(parse_connect_args({}).has_value());
    REQUIRE_FALSE(parse_connect_args({"127.0.0.1"}).has_value());
    REQUIRE_FALSE(parse_connect_args({"127.0.0.1", "8082", "a"}).has_value());
    REQUIRE_FALSE(parse_connect_args({"127.0.0.1", "port"}).has_value());
    REQUIRE_FALSE(parse_connect_args({"127.0.0.1", "8082x"}).has_value());
    REQUIRE_FALSE(parse_connect_args({"127.0.0.1", "0"}).has_value());
    REQUIRE_FALSE(parse_connect_args({"127.0.0.1", "65536"}).has_value());
}
