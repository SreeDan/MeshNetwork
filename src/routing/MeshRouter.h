#pragma once
#include <functional>
#include <string>
#include <mutex>
#include <shared_mutex>

#include "DedeupSet.h"
#include "IMessageSink.h"
#include "RpcManager.h"
#include "packet.pb.h"
#include "ThreadSafeQueue.h"

const auto ROUTE_TIMEOUT = std::chrono::seconds(180); // Neighbors expire if silent for 3 mins
const auto UPDATE_INTERVAL = std::chrono::seconds(10); // Periodic full dumps
const auto TRIGGER_DEBOUNCE = std::chrono::milliseconds(100); // Wait 100ms before sending trigger

struct RouteEntry {
    std::string next_hop; // immediate neighbor to send to
    uint32_t cost;
    bool dirty;
    std::chrono::steady_clock::time_point last_updated;
};

struct NeighborView {
    std::unordered_map<std::string, uint32_t> advertised_routes;
    std::chrono::steady_clock::time_point last_heard_from;
};

class MeshRouter : public IMessageSink {
public:
    using OnMessageReceived = std::function<void(const mesh::RoutedPacket &pkt)>;
    using OnPacketForMe = std::function<void(mesh::RoutedPacket &)>;
    static constexpr int DEFAULT_TTL = 10;

    MeshRouter(boost::asio::io_context &ioc, const std::string &self_id,
               const std::shared_ptr<ITransportLayer> &transport = nullptr);

    ~MeshRouter();

    // User API
    void start();

    void stop();

    // Config
    void set_on_packet_for_me(OnPacketForMe cb);

    void set_transport(std::shared_ptr<ITransportLayer> transport);

    // IMessageSink impl
    void push_data_bytes(const std::string &from_peer, const std::string &payload_bytes) override;

    void on_peer_connected(const std::string &peer_id) override;

    void on_peer_disconnected(const std::string &peer_id) override;

    void send_packet(mesh::RoutedPacket &pkt);

    // Could expose a method in the ITransportLayer interface to get the peer id's of the active connection,
    // but because the router is handling the logic to determine where the packets should go, we should use its view
    std::vector<std::string> get_direct_neighbors();

    std::vector<std::string> get_peers_in_network();

    // // Debug
    // void generate_topology_graph(const std::string &destination_path);

private:
    using DedupKey = std::tuple<std::string, bool>;

    const std::string self_id_;
    std::weak_ptr<ITransportLayer> transport_;
    std::atomic_bool running_{false};
    std::thread routing_thread_;


    // Callback to MeshNode on what to do with incoming packets
    OnMessageReceived on_message_cb_;

    OnPacketForMe on_packet_for_me_cb_;

    mutable std::shared_mutex mu_;
    // Routing state (protected by mu_)
    std::unordered_map<std::string, RouteEntry> forwarding_table_;
    std::unordered_map<std::string, NeighborView> neighbor_views_;

    // TODO: change for something more robust like LRU which removes old ids
    // Set based off {peer ids, request/response=true/false}
    DedupSet<DedupKey, TupleHash> seen_ids_;

    // bool ignore_messages_{false};
    struct RouterEvent {
        enum Type {
            PACKET_RECEIVED,
            PEER_CONNECTED,
            PEER_DISCONNECTED
        } type;

        std::string peer_id;
        mesh::RoutedPacket packet; // Only used for PACKET_RECEIVED
    };

    ThreadSafeQueue<RouterEvent> event_queue_;

    // Timer State
    std::chrono::steady_clock::time_point last_periodic_update;
    std::chrono::steady_clock::time_point trigger_deadline_;
    bool trigger_pending_ = false;

    static constexpr uint32_t INF_COST = 16; // Count-to-infinity guard

    void processing_loop(); // Main event loop

    void handle_packet(const std::string &src_id, const std::string &pkt_id, mesh::RoutedPacket &pkt);

    void route_data_packet(const std::string &src_peer,
                           const std::string &dest_peer,
                           const std::string &pkt_id,
                           mesh::RoutedPacket &pkt
    );


    std::vector<std::string> determine_next_hop(const std::string &src_peer, const std::string &dest_peer);

    void process_routing_update_locked(const std::string &neighbor_id, const mesh::RouteTable &table);

    void recalculate_forwarding_table_locked();

    void run_periodic_maintenance();

    void send_triggered_updates();

    void broadcast_full_table();

    // // These methods never lock
    std::vector<std::string> get_direct_neighbors_locked() const;

    std::vector<std::string> get_peers_in_network_locked();


    // Debug
    void print_routing_table();
};
