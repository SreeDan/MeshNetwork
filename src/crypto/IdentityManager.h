#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../proto_types/EnvelopeUtils.h"

namespace mesh {
    class RoutedPacket;
}

class IdentityManager {
public:
    void load_identities(const std::string &cert_path, const std::string &key_path, const std::string &ca_path);

    bool has_key(const std::string &peer_id);

    std::string get_public_key(const std::string &peer_id);

    void add_trusted_peer(const std::string &peer_id, const std::string &pub_key);

    void buffer_incoming_packet(const std::string &peer_id, mesh::RoutedPacket pkt);

    void buffer_outgoing_packet(const std::string &peer_id, mesh::RoutedPacket pkt);

    std::vector<mesh::RoutedPacket> pop_incoming_pending_packets(const std::string &peer_id);

    std::vector<mesh::RoutedPacket> pop_outgoing_pending_packets(const std::string &peer_id);

    [[nodiscard]] std::string get_my_cert() const;

    [[nodiscard]] std::string get_my_private_key() const;

    [[nodiscard]] std::string get_ca_root() const;

private:
    std::string my_cert_;
    std::string my_private_key_;
    std::string ca_root_;

    // The Whitelist / KeyStore
    std::mutex mu_;
    std::map<std::string, std::string> trusted_keys_; // PeerID -> PublicKey

    // packets that arrived before we verified the sender
    std::map<std::string, std::vector<mesh::RoutedPacket> > pending_incoming_packets_;
    std::map<std::string, std::vector<mesh::RoutedPacket> > pending_outgoing_packets_;
};
