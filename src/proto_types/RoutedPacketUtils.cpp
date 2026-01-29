#include <expected>
#include <optional>

#include "CryptoHelpers.h"
#include "EnvelopeUtils.h"
#include "Logger.h"
#include "MessageUtils.h"
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
            std::string payload,
            bool expect_response
        ) {
            RoutedPacket pkt;
            pkt.set_id(generate_uuid_bytes());
            pkt.set_from_peer_id(from_peer_id);
            pkt.set_to_peer_id(to_peer_id);
            pkt.set_ttl(ttl);
            pkt.set_type(PacketType::TEXT);
            pkt.set_text(payload);
            pkt.set_expect_response(expect_response);
            return pkt;
        }

        RoutedPacket MakeBinaryRoutedPacket(
            std::string from_peer_id,
            std::string to_peer_id,
            uint32_t ttl,
            std::string subtype,
            mesh::TransportProtocol transport,
            std::string raw_data,
            bool expect_response
        ) {
            RoutedPacket pkt;
            pkt.set_id(generate_uuid_bytes());
            pkt.set_from_peer_id(from_peer_id);
            pkt.set_to_peer_id(to_peer_id);
            pkt.set_ttl(ttl);
            pkt.set_type(PacketType::BINARY);
            pkt.set_subtype(subtype);
            pkt.set_transport(transport);
            pkt.set_binary_data(raw_data.data(), raw_data.size());
            pkt.set_expect_response(expect_response);
            return pkt;
        }

        RoutedPacket MakeRoutingTableRoutedPacket(
            std::string from_peer_id,
            std::string to_peer_id,
            uint32_t ttl,
            const std::map<std::string, uint32_t> &routing_table
        ) {
            RoutedPacket pkt;
            pkt.set_id(generate_uuid_bytes());
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
            const RouteTable &rt
        ) {
            RoutedPacket pkt;
            pkt.set_id(generate_uuid_bytes());
            pkt.set_from_peer_id(from_peer_id);
            pkt.set_to_peer_id(to_peer_id);
            pkt.set_ttl(ttl);
            pkt.set_type(PacketType::ROUTING_UPDATE);

            pkt.mutable_route_table()->CopyFrom(rt);
            return pkt;
        }

        RoutedPacket MakeEncryptedBinaryRoutedPacket(
            std::string from_peer_id,
            std::string to_peer_id,
            uint32_t ttl,
            std::string subtype,
            mesh::TransportProtocol transport,
            std::string dest_public_key,
            std::string src_private_key,
            std::string unencrypted_data,
            bool expect_response
        ) {
            RoutedPacket pkt;
            pkt.set_id(generate_uuid_bytes());
            pkt.set_from_peer_id(from_peer_id);
            pkt.set_to_peer_id(to_peer_id);
            pkt.set_ttl(ttl);
            pkt.set_type(PacketType::BINARY);
            pkt.set_subtype(subtype);
            pkt.set_transport(transport);
            try {
                std::string session_key = CryptoHelpers::generate_aes_key();

                std::string enc_payload = CryptoHelpers::aes_encrypt(session_key, unencrypted_data);
                std::string enc_key = CryptoHelpers::rsa_encrypt_key(dest_public_key, session_key);

                std::string sig = CryptoHelpers::sign_message(
                    src_private_key,
                    enc_payload
                );
            } catch (const std::exception &e) {
                throw std::runtime_error(e.what());
            }

            pkt.set_expect_response(expect_response);
            return pkt;
        }


        std::expected<std::string, std::string> decrypt_packet_data(
            const std::string &sender_pub_key,
            const std::string &dest_priv_key,
            mesh::RoutedPacket &pkt
        ) {
            const std::string &initial_src = pkt.from_peer_id();

            bool valid_sig = CryptoHelpers::verify_signature(
                sender_pub_key,
                pkt.encrypted_payload(),
                pkt.signature()
            );

            if (!valid_sig) {
                Log::error("security", {{"sender", initial_src}}, "Signature mismatch! Dropping packet.");
                return "";
            }

            try {
                std::string session_key = CryptoHelpers::rsa_decrypt_key(dest_priv_key, pkt.encrypted_session_key());
                return CryptoHelpers::aes_decrypt(session_key, pkt.encrypted_payload());
            } catch (const std::exception &e) {
                Log::error("security", {{"sender", initial_src}, {"err", e.what()}}, "Decryption failed");
                return std::unexpected(e.what());
            }
        }
    }
}
