#include <optional>

#include "packet.pb.h"

namespace mesh {
    namespace packet {
        std::optional<RoutedPacket> decodeRoutedPacket(const std::string &payload_bytes) {
            RoutedPacket pkt;
            if (!pkt.ParseFromString(payload_bytes)) {
                return std::nullopt;
            }

            return pkt;
        }

        RoutedPacket MakeTextRoutedPacket(
            std::string from_peer_id,
            std::string to_peer_id,
            uint32_t ttl,
            std::string payload
        ) {
            RoutedPacket pkt;
            pkt.set_from_peer_id(from_peer_id);
            pkt.set_to_peer_id(to_peer_id);
            pkt.set_ttl(ttl);
            pkt.set_type(PacketType::TEXT);
            pkt.set_text(payload);
            return pkt;
        }

        RoutedPacket MakeBinaryRoutedPacket(
            std::string from_peer_id,
            std::string to_peer_id,
            uint32_t ttl,
            std::vector<uint8_t> raw_data
        ) {
            RoutedPacket pkt;
            pkt.set_from_peer_id(from_peer_id);
            pkt.set_to_peer_id(to_peer_id);
            pkt.set_ttl(ttl);
            pkt.set_type(PacketType::BINARY);
            pkt.set_binary_data(raw_data.data(), raw_data.size());
            return pkt;
        }

        RoutedPacket MakeRoutingTableRoutedPacket(
            std::string from_peer_id,
            std::string to_peer_id,
            uint32_t ttl,
            std::map<std::string, uint32_t> &routing_table
        ) {
            RoutedPacket pkt;
            pkt.set_from_peer_id(from_peer_id);
            pkt.set_to_peer_id(to_peer_id);
            pkt.set_ttl(ttl);
            pkt.set_type(PacketType::ROUTING_UPDATE);

            RouteTable rt;
            rt.mutable_costs()->insert(routing_table.begin(), routing_table.end());
            pkt.set_allocated_route_table(&rt);
            return pkt;
        }
  RoutedPacket MakeRoutingTableRoutedPacket(
            std::string from_peer_id,
            std::string to_peer_id,
            uint32_t ttl,
            RouteTable *rt
        ) {
            RoutedPacket pkt;
            pkt.set_from_peer_id(from_peer_id);
            pkt.set_to_peer_id(to_peer_id);
            pkt.set_ttl(ttl);
            pkt.set_type(PacketType::ROUTING_UPDATE);

            pkt.set_allocated_route_table(rt);
            return pkt;
        }
    }
}
