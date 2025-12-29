#pragma once
#include <functional>
#include <string>
#include <mutex>
#include <shared_mutex>

#include "IMessageSink.h"
#include "MeshEvents.h"
#include "RpcManager.h"
#include "packet.pb.h"
#include "RoutedPacketUtils.h"
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

    MeshRouter(const std::string &self_id, std::shared_ptr<IMeshTransport> transport = nullptr);

    ~MeshRouter();

    void set_transport(std::shared_ptr<IMeshTransport> transport);

    // User API
    void start();

    void stop();

    void send_text(const std::string &dest_id, const std::string &text);

    void send_bytes(const std::string &dest_id, const std::vector<uint8_t> &bytes);

    void set_on_message_received(OnMessageReceived cb);

    // IMessageSink impl
    void push_data_bytes(const std::string &from_peer, const std::string &payload_bytes) override;

    void on_peer_connected(const std::string &peer_id) override;

    void on_peer_disconnected(const std::string &peer_id) override;

private:
    const std::string self_id_;
    std::weak_ptr<IMeshTransport> transport_;

    OnMessageReceived on_message_cb_;

    std::thread routing_thread_;
    std::mutex mu_;
    std::atomic_bool running_{false};

    ThreadSafeQueue<MeshEvent> event_queue_;

    // TODO: change for something more robust
    // Maybe LRU set based off peer ids
    std::unordered_set<std::string> seen_ids_;

    // Routing state (protected by mu_)
    std::unordered_map<std::string, RouteEntry> forwarding_table_;
    std::unordered_map<std::string, NeighborView> neighbor_views_;

    // Timer State
    std::chrono::steady_clock::time_point last_periodic_update;
    std::chrono::steady_clock::time_point trigger_deadline_;
    bool trigger_pending_ = false;

    void processing_loop(); // Main event loop

    void handle_packet(const std::string &src_id, mesh::RoutedPacket &pkt);

    void process_routing_update(const std::string &neighbor_id, const mesh::RouteTable &table);

    void route_data_packet(const std::string &from_peer, const std::string &dest_peer, mesh::RoutedPacket &pkt);

    // Maintenance helpers
    void recalculate_forwarding_table();

    void run_periodic_maintenance(std::chrono::steady_clock::time_point time_point);

    void send_triggered_updates();

    void broadcast_full_table();

    // Debug
    void print_routing_table();
};
