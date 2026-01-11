#pragma once

#include <optional>
#include <string>

#include "packet.pb.h"

namespace mesh {
    namespace packet {
        std::optional<RoutedPacket> decodeRoutedPacket(const std::string &payload_bytes);

        RoutedPacket MakeTextRoutedPacket(
            std::string from_peer_id,
            std::string to_peer_id,
            uint32_t ttl,
            std::string payload,
            bool expect_response = false
        );

        RoutedPacket MakeBinaryRoutedPacket(
            std::string from_peer_id,
            std::string to_peer_id,
            uint32_t ttl,
            std::string subtype,
            std::string raw_data,
            bool expect_response = false
        );

        RoutedPacket MakeRoutingTableRoutedPacket(
            std::string from_peer_id,
            std::string to_peer_id,
            uint32_t ttl,
            const std::map<std::string, uint32_t> &routing_table
        );

        RoutedPacket MakeRoutingTableRoutedPacket(
            std::string from_peer_id,
            std::string to_peer_id,
            uint32_t ttl,
            const RouteTable &rt
        );
    }
}


