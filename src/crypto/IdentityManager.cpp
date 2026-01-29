#include "IdentityManager.h"

#include "CertHelpers.h"
#include "FileUtils.h"
#include "Logger.h"
#include "packet.pb.h"


void IdentityManager::load_identities(const std::string &cert_path,
                                      const std::string &key_path,
                                      const std::string &ca_path) {
    try {
        ca_root_ = read_file_to_string(ca_path);
        my_cert_ = read_file_to_string(cert_path);
        my_private_key_ = read_file_to_string(key_path);

        // Verify our own certificate against the CA Root.
        // If this fails, we are using the wrong keys
        bool valid_chain = CertHelpers::verify_certificate(my_cert_, ca_root_);
        if (!valid_chain) {
            throw std::runtime_error("IdentityManager: Startup Check Failed "
                "My certificate is not signed by the provided CA Root.");
        }

        // auto whitelist ourselves
        std::string my_pub_key = CertHelpers::extract_pubkey_from_cert(my_cert_);
        std::string my_id = CertHelpers::extract_common_name(my_cert_);

        add_trusted_peer(my_id, my_pub_key);

        Log::info("IdentityManager", {{"peer_id", my_id}}, "successfully loaded identity");
    } catch (const std::exception &e) {
        Log::error("IdentityManager", {{"err", e.what()}}, "failed to load identity");
        throw;
    }
}

bool IdentityManager::has_key(const std::string &peer_id) {
    std::lock_guard<std::mutex> guard(mu_);
    return trusted_keys_.contains(peer_id);
}

std::optional<std::string> IdentityManager::get_public_key(const std::string &peer_id) {
    std::lock_guard<std::mutex> guard(mu_);
    if (trusted_keys_.contains(peer_id)) {
        return trusted_keys_.at(peer_id);
    }
    return std::nullopt;
}

void IdentityManager::add_trusted_peer(const std::string &peer_id, const std::string &pub_key) {
    std::lock_guard<std::mutex> guard(mu_);
    trusted_keys_[peer_id] = pub_key;
}

void IdentityManager::buffer_incoming_packet(const std::string &peer_id, mesh::RoutedPacket pkt) {
    std::lock_guard<std::mutex> guard(mu_);
    pending_incoming_packets_[peer_id].push_back(std::move(pkt));
}

void IdentityManager::buffer_outgoing_packet(const std::string &peer_id, mesh::RoutedPacket pkt) {
    std::lock_guard<std::mutex> guard(mu_);
    pending_outgoing_packets_[peer_id].push_back(std::move(pkt));
}

std::vector<mesh::RoutedPacket> IdentityManager::pop_incoming_pending_packets(const std::string &peer_id) {
    std::lock_guard<std::mutex> guard(mu_);
    auto it = pending_incoming_packets_.find(peer_id);
    if (it == pending_incoming_packets_.end()) return {};

    auto vec = std::move(it->second);
    pending_incoming_packets_.erase(it);
    return vec;
}

std::vector<mesh::RoutedPacket> IdentityManager::pop_outgoing_pending_packets(const std::string &peer_id) {
    std::lock_guard<std::mutex> guard(mu_);
    auto it = pending_outgoing_packets_.find(peer_id);
    if (it == pending_outgoing_packets_.end()) return {};

    auto vec = std::move(it->second);
    pending_outgoing_packets_.erase(it);
    return vec;
}

std::string IdentityManager::get_my_cert() const {
    return my_cert_;
}

std::string IdentityManager::get_my_private_key() const {
    return my_private_key_;
}

std::string IdentityManager::get_ca_root() const {
    return ca_root_;
}
