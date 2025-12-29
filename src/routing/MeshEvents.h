#include <string>
#include "envelope.pb.h"
#include "RoutedPacketUtils.h"

enum class EventType {
    PACKET_RECEIVED,
    PEER_CONNECTED,
    PEER_DISCONNECTED,
};

struct MeshEvent {
    EventType type;
    std::string peer_id;

    // Optional: used if type == PACKET_RECEIVED
    mesh::RoutedPacket packet;
};
