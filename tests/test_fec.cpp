#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "mesh/net/udp/FecCodec.h"

namespace {

std::vector<std::string> make_payloads() {
    return {
        "sree",
        "sree-dan",
        "",
        std::string("delta\0binary", 12),
        "echo"
    };
}

std::vector<std::string> encode_group(FecCodec& encoder, const std::vector<std::string>& payloads) {
    std::vector<std::string> frames;
    for (const auto& payload : payloads) {
        auto encoded = encoder.encode(payload);
        frames.push_back(std::move(encoded.data_frame));
        frames.insert(frames.end(),
                      std::make_move_iterator(encoded.parity_frames.begin()),
                      std::make_move_iterator(encoded.parity_frames.end()));
    }
    return frames;
}

std::vector<std::string> decode_kept_frames(const std::vector<std::string>& frames,
                                            const std::vector<int>& keep) {
    FecCodec decoder(5, 3);
    std::vector<std::string> decoded;
    for (int index : keep) {
        auto next = decoder.decode(frames.at(index));
        if (!next.empty()) {
            decoded = std::move(next);
        }
    }
    return decoded;
}

}

TEST_CASE("FecCodec marks frames with a stable magic header", "[fec]") {
    FecCodec encoder(2, 1);

    auto encoded = encoder.encode("hello");

    REQUIRE(FecCodec::is_fec_frame(encoded.data_frame));
    REQUIRE(FecCodec::frame_config(encoded.data_frame) == std::make_pair<uint8_t, uint8_t>(2, 1));
    REQUIRE_FALSE(FecCodec::is_fec_frame("raw protobuf bytes"));
    REQUIRE_FALSE(FecCodec::frame_config("raw protobuf bytes").has_value());
}

TEST_CASE("FecCodec recovers data from parity and out-of-order frames", "[fec]") {
    auto payloads = make_payloads();
    FecCodec encoder(5, 3);
    auto frames = encode_group(encoder, payloads);

    REQUIRE(frames.size() == 8);
    REQUIRE(decode_kept_frames(frames, {0, 1, 2, 3, 4}) == payloads);
    REQUIRE(decode_kept_frames(frames, {0, 2, 4, 5, 6}) == payloads);
    REQUIRE(decode_kept_frames(frames, {3, 4, 5, 6, 7}) == payloads);
    REQUIRE(decode_kept_frames(frames, {7, 6, 5, 4, 3}) == payloads);
}

TEST_CASE("FecCodec rejects invalid settings and oversized payloads", "[fec]") {
    REQUIRE_THROWS_AS(FecCodec(0, 1), std::invalid_argument);
    REQUIRE_THROWS_AS(FecCodec(4, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(FecCodec(253, 2), std::invalid_argument);

    FecCodec encoder(4, 2);
    REQUIRE_THROWS_AS(encoder.encode(std::string(65536, 'x')), std::invalid_argument);
}
