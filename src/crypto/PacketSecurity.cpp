#include "PacketSecurity.h"

#include <expected>

#include "CryptoHelpers.h"
#include "IdentityManager.h"
#include "Logger.h"
#include "packet.pb.h"

PacketSecurity::PacketSecurity(std::shared_ptr<IdentityManager> identity_manager)
    : identity_manager_(std::move(identity_manager)) {
}

bool PacketSecurity::secure_packet(mesh::RoutedPacket &pkt, const std::string &target_peer_id,
                                   const std::string &raw_payload) {
    std::string target_pub_key = identity_manager_->get_public_key(target_peer_id);

    if (target_pub_key.empty()) {
        Log::warn("PacketSecurity", {{"target", target_peer_id}}, "encryption aborted, unknown identity.");
        return false;
    }

    try {
        std::string session_key = CryptoHelpers::generate_aes_key();
        std::string enc_payload = CryptoHelpers::aes_encrypt(session_key, raw_payload);
        std::string enc_session_key = CryptoHelpers::rsa_encrypt_key(target_pub_key, session_key);

        std::string my_priv_key = identity_manager_->get_my_private_key();
        std::string signature = CryptoHelpers::sign_message(my_priv_key, enc_payload);

        pkt.set_encrypted_payload(enc_payload);
        pkt.set_encrypted_session_key(enc_session_key);
        pkt.set_signature(signature);
        pkt.clear_binary_data();

        return true;
    } catch (const std::exception &e) {
        Log::error("PacketSecurity", {{"target", target_peer_id}, {"error", e.what()}}, "encryption internal error");
        return false;
    }
}

std::expected<std::string, std::string> PacketSecurity::decrypt_packet(const mesh::RoutedPacket &pkt) {
    const std::string &sender_id = pkt.from_peer_id();

    std::string sender_pub_key = identity_manager_->get_public_key(sender_id);

    if (sender_pub_key.empty()) {
        const std::string &err = "sender identity unknown.";
        Log::warn("PacketSecurity", {{"sender", sender_id}}, err);
        return std::unexpected(err);
    }

    try {
        bool valid_sig = CryptoHelpers::verify_signature(
            sender_pub_key,
            pkt.encrypted_payload(),
            pkt.signature()
        );

        if (!valid_sig) {
            const std::string &msg = "invalid signature";
            Log::error("PacketSecurity", {{"sender", sender_id}}, msg);
            return std::unexpected(msg);
        }

        std::string my_priv_key = identity_manager_->get_my_private_key();
        std::string session_key = CryptoHelpers::rsa_decrypt_key(my_priv_key, pkt.encrypted_session_key());

        std::string plaintext = CryptoHelpers::aes_decrypt(session_key, pkt.encrypted_payload());

        return plaintext;
    } catch (const std::exception &e) {
        const std::string &msg = "decryption internal error";

        Log::error("PacketSecurity", {{"sender", sender_id}, {"error", e.what()}}, msg);
        return std::unexpected(msg);
    }
}
