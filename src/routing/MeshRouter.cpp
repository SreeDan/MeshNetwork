#include "MeshRouter.h"

#include "packet.pb.h"
#include "RoutedPacketUtils.h"

MeshRouter::MeshRouter(const std::string &self_id, std::shared_ptr<IMeshTransport> transport)
    : self_id_(self_id), transport_(transport) {
    forwarding_table_[self_id] = RouteEntry{self_id, 0, false};
    last_periodic_update = std::chrono::steady_clock::now();
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


void MeshRouter::set_on_message_received(OnMessageReceived cb) {
    on_message_cb_ = std::move(cb);
}

void MeshRouter::push_packet(const std::string &from_peer, const mesh::Envelope &env) {
    event_queue_.push({EventType::PACKET_RECEIVED, from_peer, env});
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
            std::lock_guard<std::mutex> lock(mu_);

            const auto &event = *event_opt;
            if (event.type == EventType::PEER_CONNECTED) {
                std::cout << "[Router] Peer connected " << event.peer_id << std::endl;
                neighbor_views_[event.peer_id].last_heard_from = std::chrono::steady_clock::now();
            } else if (event.type == EventType::PEER_DISCONNECTED) {
                std::cout << "[Router] Peer disconnected " << event.peer_id << std::endl;
                neighbor_views_.erase(event.peer_id);
                recalculate_forwarding_table();
            } else if (event.type == EventType::PACKET_RECEIVED) {
                handle_packet(event.peer_id, event.envelope);
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

void MeshRouter::handle_packet(const std::string &src_id, const mesh::Envelope &env) {
    // Do dedup check.
    // If seenmessages.contains(env.msg.id())

    std::optional<mesh::RoutedPacket> pkt_opt = mesh::packet::decodeRoutedPacket(env.payload());
    if (!pkt_opt.has_value()) {
        return;
    }
    mesh::RoutedPacket &pkt = pkt_opt.value();

    if (pkt.type() == mesh::PacketType::ROUTING_UPDATE) {
        process_routing_update(src_id, pkt.route_table());
    } else if (pkt.type() == mesh::PacketType::TEXT) {
        route_data_packet(pkt.from_peer_id(), pkt.to_peer_id(), pkt);
    }
}

void MeshRouter::send_text(const std::string &dest_id, const std::string &text) {
    // route_data_packet(self_id_, dest_id, text);
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

    std::lock_guard<std::mutex> lock(mu_);
    std::string next_peer_id;
    {
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
        t->send_message(next_peer_id, env);
    } else {
        std::cerr << "[Router] Transport is dead, cannot route." << std::endl;
    }
}

void MeshRouter::process_routing_update(const std::string &neighbor_id, const mesh::RouteTable &table) {
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
    new_table[self_id_] = RouteEntry{self_id_, 0, false};

    for (const auto &[neighbor_id, view]: neighbor_views_) {
        if (!new_table.contains(neighbor_id) || 1 < new_table[neighbor_id].cost) {
            new_table[neighbor_id] = {neighbor_id, 1, true};
        }

        for (const auto &[dest, remote_cost]: view.advertised_routes) {
            const uint32_t total_cost = remote_cost + 1;
            if (total_cost >= INF_COST) continue;

            if (!new_table.contains(dest) || total_cost < new_table[dest].cost) {
                new_table[dest] = {neighbor_id, total_cost, true};
            }
        }
    }

    // merge new_table into forwarding_table
    auto now = std::chrono::steady_clock::now();

    for (const auto &[dest, entry]: new_table) {
        auto it = forwarding_table_.find(dest);
        if (it == forwarding_table_.end()) {
            forwarding_table_[dest] = entry;
            forwarding_table_[dest].last_updated = now;
            forwarding_table_[dest].dirty = true;
            table_changed = true;
        } else if (it->second.cost != entry.cost || it->second.next_hop != entry.next_hop) {
            forwarding_table_[dest] = entry;
            forwarding_table_[dest].last_updated = now;
            forwarding_table_[dest].dirty = true;
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

// void MeshRouter::on_packet_received(const mesh::Envelope &env) {
//     std::string src_peer_id;
//     {
//         // Deal with dedeuplication of seen messages
//     }
//
//     // if (env.type() == mesh::EnvelopeType::ROUTING_UPDATE) {
//     //     mesh::RouteTable rt;
//     //     if (rt.ParseFromString(env.payload())) {
//     //         std::lock_guard<std::mutex> lock(mu_);
//     //         process_routing_update(src_peer_id, rt);
//     //     }
//     // } else if (env.type() == mesh::EnvelopeType::CUSTOM_TEXT) {
//     //     mesh::RoutedPacket pkt;
//     //     if (pkt.ParseFromString(env.payload())) {
//     //         // No lock needed here (route_data_packet locks internally when needed)
//     //         route_data_packet(pkt.from_peer_id(), pkt.to_peer_id(), pkt.text());
//     //     }
//     // }
// }

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
    std::unordered_map<std::string, uint32_t> dirty_routes;

    for (auto &[dest, entry]: forwarding_table_) {
        if (entry.dirty) {
            dirty_routes[dest] = entry.cost;
            entry.dirty = false;
        }
    }
    if (dirty_routes.empty()) return;

    for (const auto &[neighbor_id, view]: neighbor_views_) {
        mesh::RouteTable update;
        for (const auto &[dest, cost]: dirty_routes) {
            uint32_t final_cost = cost;

            // poison reverse
            auto it = forwarding_table_.find(dest);
            if (it != forwarding_table_.end() && it->second.next_hop == neighbor_id) {
                final_cost = INF_COST;
            }

            (*update.mutable_costs())[dest] = final_cost;
        }

        mesh::Envelope env;
        env.set_type(mesh::EnvelopeType::DATA);
        auto pkt = mesh::packet::MakeRoutingTableRoutedPacket(
            self_id_,
            neighbor_id,
            15,
            &update
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

            (*update.mutable_costs())[dest] = final_cost;
        }

        mesh::Envelope env;
        env.set_type(mesh::EnvelopeType::DATA);
        auto pkt = mesh::packet::MakeRoutingTableRoutedPacket(
            self_id_,
            neighbor_id,
            15,
            &update
        );

        env.set_payload(pkt.SerializeAsString());

        if (std::shared_ptr t = transport_.lock()) {
            t->send_message(neighbor_id, env);
        }
    }
}
