#pragma once
#include <functional>
#include <string>
#include <mutex>
#include <shared_mutex>

#include "AsyncRequestTracker.h"
#include "DedeupSet.h"
#include "IMessageSink.h"
#include "MeshEvents.h"
#include "RpcManager.h"
#include "packet.pb.h"
#include "ThreadSafeQueue.h"

const uint32_t DEFAULT_TTL = 10;
const uint32_t INF_COST = 16;
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
    using OnMessageReceived = std::function<void(const std::string &from_id, const mesh::RoutedPacket &pkt)>;

    MeshRouter(boost::asio::io_context &ioc, const std::string &self_id,
               const std::shared_ptr<ITransportLayer> &transport = nullptr);

    ~MeshRouter();

    void set_transport(std::shared_ptr<ITransportLayer> transport);

    // User API
    void start();

    void stop();

    void send_packet(mesh::RoutedPacket &pkt);

    void send_text(const std::string &dest_id, const std::string &text);

    void send_bytes(const std::string &dest_id, const std::vector<uint8_t> &bytes);

    std::future<std::string> send_request(mesh::RoutedPacket &pkt, std::chrono::milliseconds timeout);

    void send_broadcast_request(mesh::RoutedPacket &pkt,
                                std::chrono::milliseconds duration,
                                std::function<void(const std::string &, std::string)> on_response,
                                const std::function<void()> &on_complete);

    void set_on_message_received(OnMessageReceived cb);

    // IMessageSink impl
    void push_data_bytes(const std::string &from_peer, const std::string &payload_bytes) override;

    void on_peer_connected(const std::string &peer_id) override;

    void on_peer_disconnected(const std::string &peer_id) override;

    // Could expose a method in the ITransportLayer interface to get the peer id's of the active connection,
    // but because the router is handling the logic to determine where the packets should go, we should use its view
    std::vector<std::string> get_direct_neighbors();

    std::vector<std::string> determine_next_hop(const std::string &src_peer, const std::string &dest_peer);

    std::vector<std::string> get_peers_in_network();

    void set_ignore_messages(bool ignore);

    // Debug
    void generate_topology_graph(const std::string &destination_path);

private:
    using DedupKey = std::tuple<std::string, bool>;

    const std::string self_id_;
    std::weak_ptr<ITransportLayer> transport_;

    OnMessageReceived on_message_cb_;

    std::thread routing_thread_;
    mutable std::shared_mutex mu_;
    std::atomic_bool running_{false};

    bool ignore_messages_{false};

    ThreadSafeQueue<MeshEvent> event_queue_;

    // TODO: change for something more robust like LRU which removes old ids
    // Set based off {peer ids, request/response=true/false}
    DedupSet<DedupKey, TupleHash> seen_ids_;

    // Routing state (protected by mu_)
    std::unordered_map<std::string, RouteEntry> forwarding_table_;
    std::unordered_map<std::string, NeighborView> neighbor_views_;

    // Timer State
    std::chrono::steady_clock::time_point last_periodic_update;
    std::chrono::steady_clock::time_point trigger_deadline_;
    bool trigger_pending_ = false;
    AsyncRequestTracker<std::string> request_tracker_;

    void processing_loop(); // Main event loop

    void handle_packet(const std::string &src_id, const std::string &pkt_id, mesh::RoutedPacket &pkt);

    void route_data_packet(const std::string &src_peer,
                           const std::string &dest_peer,
                           const std::string &pkt_id,
                           mesh::RoutedPacket &pkt
    );

    void run_periodic_maintenance(std::chrono::steady_clock::time_point time_point);

    void send_triggered_updates();

    void broadcast_full_table();

    // These methods never lock
    std::vector<std::string> get_direct_neighbors_locked() const;

    std::vector<std::string> get_peers_in_network_locked();

    std::vector<std::string> determine_next_hop_locked(const std::string &src_peer, const std::string &dest_peer) const;

    void recalculate_forwarding_table_locked();

    void process_routing_update_locked(const std::string &neighbor_id, const mesh::RouteTable &table);


    // Debug
    void print_routing_table();
};
