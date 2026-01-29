#pragma once

#include <expected>
#include <memory>

#include "IdentityManager.h"

enum PacketSecurityError {
    DestPublicKeyNotFound
};

class PacketSecurity {
public:
    explicit PacketSecurity(std::shared_ptr<IdentityManager> identity_manager);

    bool secure_packet(mesh::RoutedPacket &pkt, const std::string &target_id, const std::string &payload);

    std::expected<std::string, std::string> decrypt_packet(const mesh::RoutedPacket &pkt);

private:
    std::shared_ptr<IdentityManager> identity_manager_;
};
