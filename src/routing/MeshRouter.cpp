#include "MeshRouter.h"

#include "packet.pb.h"
#include "RoutedPacketUtils.h"

MeshRouter::MeshRouter(const std::string &self_id, std::shared_ptr<IMeshTransport> transport)
    : self_id_(self_id), transport_(transport) {
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

void MeshRouter::set_transport(std::shared_ptr<IMeshTransport> transport) {
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
                std::cout << "[Router] Peer connected " << event.peer_id << std::endl;
                neighbor_views_[event.peer_id].last_heard_from = std::chrono::steady_clock::now();
                recalculate_forwarding_table();
            } else if (event.type == EventType::PEER_DISCONNECTED) {
                std::cout << "[Router] Peer disconnected " << event.peer_id << std::endl;
                neighbor_views_.erase(event.peer_id);
                recalculate_forwarding_table();
            } else if (event.type == EventType::PACKET_RECEIVED) {
                handle_packet(event.peer_id, event.packet);
            }
        }


        now = std::chrono::steady_clock::now();
        if (now >= next_maintenance_time) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                run_periodic_maintenance(std::chrono::steady_clock::time_point());
            }
            next_maintenance_time = now + MAINTENANCE_TICK_RATE;
        }
    }
}

void MeshRouter::handle_packet(const std::string &src_id, mesh::RoutedPacket &pkt) {
    // Do dedup check.
    // If seenmessages.contains(env.msg.id())
    if (src_id == self_id_ || pkt.from_peer_id() == self_id_) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (neighbor_views_.contains(src_id)) {
            neighbor_views_[src_id].last_heard_from = std::chrono::steady_clock::now();
        }
    }

    if (pkt.type() == mesh::PacketType::ROUTING_UPDATE) {
        process_routing_update(src_id, pkt.route_table());
    } else if (pkt.type() == mesh::PacketType::TEXT) {
        route_data_packet(pkt.from_peer_id(), pkt.to_peer_id(), pkt);
    } else {
        std::cerr << "Unknown type " << pkt.type() << std::endl;
    }
}

void MeshRouter::send_text(const std::string &dest_id, const std::string &text) {
    mesh::RoutedPacket pkt = mesh::packet::MakeTextRoutedPacket(
        self_id_,
        dest_id,
        DEFAULT_TTL,
        text
    );
    route_data_packet(self_id_, dest_id, pkt);
}

void MeshRouter::route_data_packet(const std::string &from_peer, const std::string &dest_peer,
                                   mesh::RoutedPacket &pkt) {
    if (dest_peer == self_id_) {
        std::cout << "[Router] Message for ME from " << from_peer << std::endl;
        if (on_message_cb_) {
            // Deliver to user
            on_message_cb_(from_peer, pkt);
        }
        return;
    }

    // Track visited peers to prevent loops
    if (seen_ids_.contains(pkt.id())) {
        std::cerr << "[Router] Detected loop for pkt " << pkt.id() << " — dropping\n";
        return;
    }
    seen_ids_.insert(pkt.id());

    std::string next_peer_id;
    {
        std::lock_guard<std::mutex> lock(mu_);
        // Find where to forward it
        auto it = forwarding_table_.find(dest_peer);
        if (it == forwarding_table_.end() || it->second.cost >= INF_COST) {
            // Only logging if we are the sender
            if (pkt.from_peer_id() == self_id_) {
                std::cerr << "Router: no route to " << dest_peer << std::endl;
            }
            return;
        }

        next_peer_id = it->second.next_hop;
    }

    if (next_peer_id == self_id_) {
        std::cerr << "[Router] Loop detected: route to "
                << dest_peer << " points to self — dropping\n";
        return;
    }

    // Construct the new env to forward
    mesh::Envelope env;
    env.set_type(mesh::EnvelopeType::DATA);
    pkt.set_ttl(pkt.ttl() - 1);
    if (pkt.ttl() < 1) {
        std::cerr << "Router: dropping pkt, ttl reached" << std::endl;
        return;
    }
    env.set_payload(pkt.SerializeAsString());

    if (std::shared_ptr<IMeshTransport> t = transport_.lock()) {
        std::cout << "routing packet through " << next_peer_id << std::endl;
        std::cout << "passing env to transport with pkt id " << pkt.id() << std::endl;
        t->send_message(next_peer_id, env);
    } else {
        std::cerr << "[Router] Transport is dead, cannot route." << std::endl;
    }
}

void MeshRouter::process_routing_update(const std::string &neighbor_id, const mesh::RouteTable &table) {
    std::lock_guard<std::mutex> lock(mu_);
    auto &view = neighbor_views_[neighbor_id];
    view.last_heard_from = std::chrono::steady_clock::now();

    for (const auto &[dest, cost]: table.costs()) {
        if (cost >= INF_COST) {
            view.advertised_routes.erase(dest);
        } else {
            view.advertised_routes[dest] = cost;
        }
    }

    recalculate_forwarding_table();
}

void MeshRouter::recalculate_forwarding_table() {
    bool table_changed = false;
    std::unordered_map<std::string, RouteEntry> new_table;
    auto now = std::chrono::steady_clock::now();
    new_table[self_id_] = {self_id_, 0, false, now};

    for (const auto &[neighbor_id, view]: neighbor_views_) {
        new_table[neighbor_id] = {neighbor_id, 1, true, now};
    }
    // if (!new_table.contains(neighbor_id) || 1 < new_table[neighbor_id].cost) {
    //     new_table[neighbor_id] = {neighbor_id, 1, true};
    // }

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
                std::cout << "Router: route lost to " << pair.first << std::endl;
            }
        }
    }

    if (table_changed && !trigger_pending_) {
        trigger_pending_ = true;
        trigger_deadline_ = now + TRIGGER_DEBOUNCE;
    }
}

void MeshRouter::run_periodic_maintenance(std::chrono::steady_clock::time_point time_point) {
    auto now = std::chrono::steady_clock::now();

    bool has_expired_neighbor = false;
    auto it = neighbor_views_.begin();
    while (it != neighbor_views_.end()) {
        if (now - it->second.last_heard_from > ROUTE_TIMEOUT) {
            std::cout << "Router: neighbor timeout " << it->first << std::endl;
            it = neighbor_views_.erase(it);
            has_expired_neighbor = true;
        } else {
            ++it;
        }
    }

    if (has_expired_neighbor) recalculate_forwarding_table();

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
                std::cout << "[Router] GC: Removing dead route to " << gc_it->first << std::endl;
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
        std::cout << "=== Routing Table (" << self_id_ << ") ===\n";
        for (const auto &[dest, entry]: forwarding_table_) {
            std::cout << "Dest: " << dest
                    << " | NextHop: " << entry.next_hop
                    << " | Cost: " << entry.cost << "\n";
        }
        std::cout << "================================\n";
    }
}
