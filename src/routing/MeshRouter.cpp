#include "MeshRouter.h"

#include <utility>

#include "GraphManager.h"
#include "Logger.h"
#include "packet.pb.h"
#include "RoutedPacketUtils.h"
#include "string_utils.h"

MeshRouter::MeshRouter(boost::asio::io_context &ioc, const std::string &self_id,
                       const std::shared_ptr<ITransportLayer> &transport)
    : self_id_(self_id), transport_(transport), request_tracker_(ioc) {
    forwarding_table_[self_id] = RouteEntry{self_id, 0, false};
    last_periodic_update = std::chrono::steady_clock::now();
    std::thread(&MeshRouter::print_routing_table, this).detach();
    start();
}

MeshRouter::~MeshRouter() {
    stop();
}

void MeshRouter::start() {
    if (running_) return;
    running_ = true;
    routing_thread_ = std::thread(&MeshRouter::processing_loop, this);
}

void MeshRouter::stop() {
    running_ = false;
    if (routing_thread_.joinable())
        routing_thread_.join();
}

void MeshRouter::set_transport(std::shared_ptr<ITransportLayer> transport) {
    transport_ = transport;
}

void MeshRouter::set_on_message_received(OnMessageReceived cb) {
    on_message_cb_ = std::move(cb);
}

void MeshRouter::push_data_bytes(const std::string &from_peer, const std::string &payload_bytes) {
    std::optional<mesh::RoutedPacket> pkt_opt = mesh::packet::decodeRoutedPacket(payload_bytes);
    if (!pkt_opt.has_value()) return;
    mesh::RoutedPacket &pkt = pkt_opt.value();

    event_queue_.push({EventType::PACKET_RECEIVED, from_peer, std::move(pkt)});
}

void MeshRouter::on_peer_connected(const std::string &peer_id) {
    event_queue_.push({EventType::PEER_CONNECTED, peer_id, {}});
}

void MeshRouter::on_peer_disconnected(const std::string &peer_id) {
    event_queue_.push({EventType::PEER_DISCONNECTED, peer_id, {}});
}

std::vector<std::string> MeshRouter::get_direct_neighbors() {
    std::shared_lock lock(mu_);
    return get_direct_neighbors_locked();
}

std::vector<std::string> MeshRouter::get_direct_neighbors_locked() const {
    std::vector<std::string> neighbors;
    neighbors.reserve(neighbor_views_.size());
    for (const auto &[id, _]: neighbor_views_) {
        if (id != self_id_) {
            neighbors.push_back(id);
        }
    }

    return neighbors;
}

std::vector<std::string> MeshRouter::get_peers_in_network() {
    std::shared_lock lock(mu_);
    return get_peers_in_network_locked();
}

std::vector<std::string> MeshRouter::get_peers_in_network_locked() {
    std::vector<std::string> peers;
    peers.reserve(forwarding_table_.size());
    for (const auto &peer: forwarding_table_ | std::views::keys) {
        peers.push_back(peer);
    }

    return peers;
}


void MeshRouter::processing_loop() {
    const auto MAINTENANCE_TICK_RATE = std::chrono::milliseconds(100);
    auto next_maintenance_time = std::chrono::steady_clock::now() + MAINTENANCE_TICK_RATE;

    while (running_) {
        auto now = std::chrono::steady_clock::now();

        // Calculate how long to wait for a packet.
        auto wait_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            next_maintenance_time - now
        );
        if (wait_duration.count() < 0) wait_duration = std::chrono::milliseconds(0);


        auto event_opt = event_queue_.pop_for(wait_duration);

        if (event_opt) {
            auto &event = *event_opt;
            if (event.type == EventType::PEER_CONNECTED) {
                Log::info("router", {{"peer_id", event.peer_id}}, "peer connected");
                std::unique_lock lock(mu_);
                neighbor_views_[event.peer_id].last_heard_from = std::chrono::steady_clock::now();
                recalculate_forwarding_table_locked();
            } else if (event.type == EventType::PEER_DISCONNECTED) {
                Log::info("router", {{"peer_id", event.peer_id}}, "peer disconnected");
                std::unique_lock lock(mu_);
                neighbor_views_.erase(event.peer_id);
                recalculate_forwarding_table_locked();
            } else if (event.type == EventType::PACKET_RECEIVED) {
                handle_packet(event.peer_id, to_hex(event.packet.id()), event.packet);
            }
        }


        now = std::chrono::steady_clock::now();
        if (now >= next_maintenance_time) {
            run_periodic_maintenance(std::chrono::steady_clock::time_point());
            next_maintenance_time = now + MAINTENANCE_TICK_RATE;
        }
    }
}

void MeshRouter::handle_packet(const std::string &src_id, const std::string &pkt_id, mesh::RoutedPacket &pkt) {
    if (src_id == self_id_ || pkt.from_peer_id() == self_id_) {
        return;
    }

    {
        std::unique_lock lock(mu_);
        if (neighbor_views_.contains(src_id)) {
            neighbor_views_[src_id].last_heard_from = std::chrono::steady_clock::now();
        }
    }

    if (pkt.type() == mesh::PacketType::ROUTING_UPDATE) {
        std::unique_lock lock(mu_);
        process_routing_update_locked(src_id, pkt.route_table());
    } else if (pkt.type() == mesh::PacketType::TEXT || pkt.type() == mesh::PacketType::BINARY) {
        route_data_packet(src_id, pkt.to_peer_id(), to_hex(pkt.id()), pkt);
    } else {
        Log::warn("handle_packet", {{"type", pkt.type()}}, "unknown packet type");
    }
}

void MeshRouter::send_packet(mesh::RoutedPacket &pkt) {
    route_data_packet(self_id_, pkt.to_peer_id(), to_hex(pkt.id()), pkt);
}

void MeshRouter::send_text(const std::string &dest_id, const std::string &text) {
    mesh::RoutedPacket pkt = mesh::packet::MakeTextRoutedPacket(
        self_id_,
        dest_id,
        DEFAULT_TTL,
        text,
        false
    );
    route_data_packet(self_id_, dest_id, to_hex(pkt.id()), pkt);
}

std::future<std::string> MeshRouter::send_request(mesh::RoutedPacket &pkt, std::chrono::milliseconds timeout) {
    std::string req_id = to_hex(pkt.id());
    pkt.set_expect_response(true);

    // TODO: make timeout a constant
    auto future = request_tracker_.track_request(req_id, std::chrono::seconds(5));

    route_data_packet(self_id_, pkt.to_peer_id(), req_id, pkt);
    return future;
}

void MeshRouter::send_broadcast_request(mesh::RoutedPacket &pkt,
                                        std::chrono::milliseconds duration,
                                        std::function<void(const std::string &, std::string)> on_response,
                                        const std::function<void()> &on_complete) {
    pkt.set_expect_response(true);
    const std::string &pkt_id = to_hex(pkt.id());
    request_tracker_.track_broadcast(pkt_id, duration, std::move(on_response), on_complete);
    route_data_packet(self_id_, "", pkt_id, pkt);
}

std::vector<std::string> MeshRouter::determine_next_hop(const std::string &src_peer, const std::string &dest_peer) {
    std::shared_lock lock(mu_);
    return determine_next_hop_locked(src_peer, dest_peer);
}

std::vector<std::string> MeshRouter::determine_next_hop_locked(const std::string &src_peer,
                                                               const std::string &dest_peer) const {
    if (dest_peer.empty()) {
        return get_direct_neighbors_locked();
    }

    // if destination is specified, find the stored next hop
    auto it = forwarding_table_.find(dest_peer);
    if (it == forwarding_table_.end() || it->second.cost >= INF_COST) {
        // Only logging if we are the sender
        if (src_peer == self_id_) {
            Log::warn("router", {{"peer_id", dest_peer}}, "no route to peer");
        }
        return {};
    }

    return {it->second.next_hop};
}

void MeshRouter::route_data_packet(const std::string &immediate_src, const std::string &dest_peer,
                                   const std::string &pkt_id, mesh::RoutedPacket &pkt) {
    const std::string &initial_src = pkt.from_peer_id();
    bool is_broadcast = dest_peer.empty();
    bool is_for_me = {dest_peer == self_id_};

    if (is_for_me && pkt.type() == mesh::PacketType::BINARY) {
        if (request_tracker_.fulfill_request(pkt_id, pkt.from_peer_id(), pkt.binary_data())) {
            return;
        }
    }

    if (is_broadcast || is_for_me) {
        bool is_my_own_msg = (initial_src == self_id_);

        if (on_message_cb_ && !is_my_own_msg) {
            on_message_cb_(initial_src, pkt);
        }

        if (is_for_me) return;
    }

    auto dedup_packet_tup = std::make_tuple(pkt_id, pkt.expect_response());

    if (immediate_src != self_id_ && seen_ids_.contains(dedup_packet_tup)) {
        // Only log if it wasn't a broadcast (broadcasts loop frequently by definition)
        if (!is_broadcast) {
            Log::warn("router", {{"packet_id", pkt_id}}, "dropping packet, loop detected");
        }
        return;
    }
    seen_ids_.insert(dedup_packet_tup);

    std::vector<std::string> next_peer_ids = determine_next_hop(immediate_src, dest_peer);

    for (const std::string &next_peer_id: next_peer_ids) {
        if (next_peer_id == immediate_src) continue;

        if (next_peer_id == self_id_) continue;

        // Copying so we don't share TTL state
        mesh::RoutedPacket forward_pkt = pkt;
        forward_pkt.set_ttl(forward_pkt.ttl() - 1);

        if (forward_pkt.ttl() <= 0) {
            Log::warn("router", {{"packet_id", pkt_id}}, "dropping packet, TTL expired for packet");
            continue;
        };


        mesh::Envelope env = mesh::envelope::MakeGenericData(forward_pkt.SerializeAsString());

        if (std::shared_ptr<ITransportLayer> t = transport_.lock()) {
            t->send_message(next_peer_id, env);
        } else {
            Log::error("router", {}, "cannot route, transport is dead");
        }
    }
}

void MeshRouter::process_routing_update_locked(const std::string &neighbor_id, const mesh::RouteTable &table) {
    auto &view = neighbor_views_[neighbor_id];
    view.last_heard_from = std::chrono::steady_clock::now();

    for (const auto &[dest, cost]: table.costs()) {
        if (cost >= INF_COST) {
            view.advertised_routes.erase(dest);
        } else {
            view.advertised_routes[dest] = cost;
        }
    }

    recalculate_forwarding_table_locked();
}

void MeshRouter::recalculate_forwarding_table_locked() {
    bool table_changed = false;
    std::unordered_map<std::string, RouteEntry> new_table;
    auto now = std::chrono::steady_clock::now();
    new_table[self_id_] = {self_id_, 0, false, now};

    for (const auto &[neighbor_id, view]: neighbor_views_) {
        new_table[neighbor_id] = {neighbor_id, 1, true, now};
    }

    for (const auto &[neighbor_id, view]: neighbor_views_) {
        for (const auto &[dest, remote_cost]: view.advertised_routes) {
            if (dest == self_id_) continue;

            const uint32_t total_cost = remote_cost + 1;
            if (total_cost >= INF_COST) continue;

            if (!new_table.contains(dest) || total_cost < new_table[dest].cost) {
                new_table[dest] = {neighbor_id, total_cost, true, now};
            }
        }
    }

    // merge new_table into forwarding_table and mark dirty if changed
    for (auto &[dest, entry]: new_table) {
        auto it = forwarding_table_.find(dest);

        if (it == forwarding_table_.end()) {
            // new route
            forwarding_table_[dest] = entry;
            forwarding_table_[dest].dirty = true;
            forwarding_table_[dest].last_updated = now;
            table_changed = true;
        } else if (it->second.cost != entry.cost || it->second.next_hop != entry.next_hop) {
            // route changed
            it->second.cost = entry.cost;
            it->second.next_hop = entry.next_hop;
            it->second.last_updated = now;
            it->second.dirty = true;
            table_changed = true;
        }
    }

    // check for lost routes
    for (auto &pair: forwarding_table_) {
        if (!new_table.contains(pair.first)) {
            if (pair.second.cost < INF_COST) {
                pair.second.cost = INF_COST;
                pair.second.dirty = true;
                pair.second.last_updated = now;
                table_changed = true;
                Log::debug("router", {{"peer_id", pair.first}}, "route lost");
            }
        }
    }

    if (table_changed && !trigger_pending_) {
        trigger_pending_ = true;
        trigger_deadline_ = now + TRIGGER_DEBOUNCE;
    }
}

void MeshRouter::run_periodic_maintenance(std::chrono::steady_clock::time_point time_point) {
    std::unique_lock lock(mu_);
    auto now = std::chrono::steady_clock::now();

    bool has_expired_neighbor = false;
    auto it = neighbor_views_.begin();
    while (it != neighbor_views_.end()) {
        if (now - it->second.last_heard_from > ROUTE_TIMEOUT) {
            Log::warn("router", {{"peer_id", it->first}}, "neighbor timeout");
            it = neighbor_views_.erase(it);
            has_expired_neighbor = true;
        } else {
            ++it;
        }
    }

    if (has_expired_neighbor) recalculate_forwarding_table_locked();

    // Debounce for changes
    if (trigger_pending_ && now >= trigger_deadline_) {
        send_triggered_updates();
        trigger_pending_ = false;
    }
    // Periodic full table broadcast
    if (now - last_periodic_update > UPDATE_INTERVAL) {
        broadcast_full_table();
        last_periodic_update = now;
    }

    auto gc_it = forwarding_table_.begin();
    while (gc_it != forwarding_table_.end()) {
        if (gc_it->second.cost >= INF_COST) {
            // Keep dead route for 15s so the INF propagates to others
            if (now - gc_it->second.last_updated > std::chrono::seconds(15)) {
                Log::debug("router", {{"peer_id", gc_it->first}}, "removing dead route");
                gc_it = forwarding_table_.erase(gc_it);
                continue;
            }
        }
        ++gc_it;
    }

    // TODO: Remove seen messages inside the DEDUP window
}

void MeshRouter::send_triggered_updates() {
    std::unordered_map<std::string, uint32_t> routes_to_send;

    for (auto &[dest, entry]: forwarding_table_) {
        if (entry.dirty || entry.cost >= INF_COST) {
            routes_to_send[dest] = entry.cost;
            entry.dirty = false;
        }
    }
    if (routes_to_send.empty()) return;

    for (const auto &[neighbor_id, view]: neighbor_views_) {
        mesh::RouteTable update;
        for (const auto &[dest, cost]: routes_to_send) {
            uint32_t final_cost = cost;

            // poison reverse, tell neighbor that route through it is INF
            auto it = forwarding_table_.find(dest);
            if (it != forwarding_table_.end() && it->second.next_hop == neighbor_id) {
                final_cost = INF_COST;
            }

            update.mutable_costs()->insert({dest, final_cost});
        }

        mesh::Envelope env;
        env.set_type(mesh::EnvelopeType::DATA);
        auto pkt = mesh::packet::MakeRoutingTableRoutedPacket(
            self_id_,
            neighbor_id,
            DEFAULT_TTL,
            update
        );

        env.set_payload(pkt.SerializeAsString());

        if (std::shared_ptr t = transport_.lock()) {
            t->send_message(neighbor_id, env);
        }
    }
}


void MeshRouter::broadcast_full_table() {
    // send our table to everyone
    for (const auto &[neighbor_id, view]: neighbor_views_) {
        mesh::RouteTable update;
        for (const auto &[dest, entry]: forwarding_table_) {
            uint32_t final_cost = entry.cost;
            // poison reverse check
            if (entry.next_hop == neighbor_id) {
                final_cost = INF_COST;
            }

            update.mutable_costs()->insert({dest, final_cost});
            // (*update.mutable_costs())[dest] = final_cost;
        }

        mesh::Envelope env;
        env.set_type(mesh::EnvelopeType::DATA);
        auto pkt = mesh::packet::MakeRoutingTableRoutedPacket(
            self_id_,
            neighbor_id,
            15,
            update
        );

        env.set_payload(pkt.SerializeAsString());

        if (std::shared_ptr t = transport_.lock()) {
            t->send_message(neighbor_id, env);
        }
    }
}

void MeshRouter::print_routing_table() {
    while (true) {
        using namespace std::chrono_literals;
        std::this_thread::sleep_for(10s);
        {
            std::shared_lock lock(mu_);

            json table_dump = json::array();

            for (const auto &[dest, entry]: forwarding_table_) {
                table_dump.push_back({
                    {"destination", dest},
                    {"next_hop", entry.next_hop},
                    {"cost", entry.cost}
                });
            }
            Log::debug("RoutingManager", {{"routing_table", table_dump}}, "Routing table update");
        }
    }
}
