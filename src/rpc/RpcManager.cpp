#include "RpcManager.h"

#include <expected>
#include <utility>
#include <boost/asio/connect.hpp>
#include <boost/proto/transform/env.hpp>

#include "EnvelopeUtils.h"
#include "Logger.h"
#include "handlers/HandshakeHandler.h"
#include "handlers/HeartbeatHandler.h"
#include "packet.pb.h"

RpcManager::RpcManager(boost::asio::io_context &ioc,
                       const std::string &peer_id,
                       int port,
                       std::shared_ptr<IMessageSink> sink,
                       std::shared_ptr<boost::asio::ssl::context> ssl_ctx)
    : ioc_(ioc),
      port_(port),
      acceptor_(ioc),
      peer_id_(peer_id),
      sink_(sink),
      ssl_ctx_(std::move(ssl_ctx)) {
    register_handlers();
    using namespace std::chrono_literals;
    heartbeat_thread_ = std::thread([this] { send_heartbeats(300ms); });
    run_maintenance_cycle();
    start_listening();
}

void RpcManager::register_handlers() {
    handlers_[mesh::EnvelopeType::HANDSHAKE] = std::make_unique<HandshakeHandler>();
    handlers_[mesh::EnvelopeType::HEARTBEAT] = std::make_unique<HeartbeatHandler>();
}

RpcManager::~RpcManager() = default;

void RpcManager::start_listening() {
    try {
        boost::system::error_code ec;

        acceptor_.open(boost::asio::ip::tcp::v4(), ec);
        if (ec) throw std::runtime_error("open failed: " + ec.message());

        // Helps when restarting after crashes / TIME_WAIT
        acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);

        acceptor_.bind(
            boost::asio::ip::tcp::endpoint(
                boost::asio::ip::tcp::v4(), port_),
            ec);

        if (ec)
            throw std::runtime_error(
                "bind failed on port " + std::to_string(port_) +
                ": " + ec.message());

        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec) throw std::runtime_error("listen failed: " + ec.message());
        Log::info("RpcManager", {{"port", port_}}, "Listening for connections");
        do_accept();
    } catch (std::exception &e) {
        Log::error("RpcManager", {{"error", e.what()}}, "Failed to start listening");
    }
}

void RpcManager::shutdown() {
}


void RpcManager::do_accept() {
    acceptor_.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket sock) {
        if (ec == boost::asio::error::operation_aborted) return;

        if (!ec) {
            boost::system::error_code ec2;
            auto remote_ep = sock.remote_endpoint(ec2);
            if (!ec2) {
                auto remote_addr = remote_ep.address().to_string();
                auto remote_port = remote_ep.port();
                Log::info("mesh_node.acceptor",
                          {{"from", remote_addr}, {"port", remote_port}},
                          "incoming connection");

                handle_new_connection(std::move(sock), false);
            }
        } else {
            Log::error("acceptor", {{"err", ec.message()}}, "accept failed");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        do_accept();
    });
}

void RpcManager::set_sink(const std::shared_ptr<IMessageSink> &sink) {
    sink_ = sink;
}

// std::expected<std::string, std::string> RpcManager::create_connection(const std::string &remote_addr,
//                                                                       boost::asio::ip::tcp::socket sock) {
//     auto rpc_connection = std::make_shared<RpcConnection>(ioc_, std::move(sock), peer_id_, ssl_ctx_);
//     rpc_connection->set_on_dispatch([this](auto conn, const auto &env) {
//         this->dispatch_message(conn, env);
//     });
//
//     // Safe to block the main thread while waiting for a handshake
//     auto res = rpc_connection->start(true); // Blocking call
//     if (res.has_value()) {
//         auto remote_peer_id = res.value().peer_id();
//         rpc_connection->set_remote_peer_id(remote_peer_id);
//         add_connection_internal(remote_peer_id, rpc_connection);
//
//
//         return remote_peer_id;
//     } else {
//         return std::unexpected(res.error());
//     }
// }

std::expected<std::string, std::string> RpcManager::connect(const std::string &host, int port) {
    boost::asio::ip::tcp::socket sock(ioc_);
    boost::asio::ip::tcp::resolver resolver(ioc_);

    auto endpoints = resolver.resolve(host, std::to_string(port));
    boost::asio::connect(sock, endpoints);
    return handle_new_connection(std::move(sock), true);
}

std::expected<std::string, std::string> RpcManager::handle_connection_startup(std::shared_ptr<RpcConnection> conn,
                                                                              bool initiator) {
    auto res = conn->start(initiator); // Blocking call
    if (res.has_value()) {
        const std::string &remote_peer_id = res.value().peer_id();
        conn->set_remote_peer_id(remote_peer_id);
        add_connection_internal(remote_peer_id, conn);
        return remote_peer_id;
    } else {
        return std::unexpected(res.error());
    }
}

std::expected<std::string, std::string> RpcManager::handle_new_connection(boost::asio::ip::tcp::socket socket,
                                                                          bool initiator) {
    auto rpc_connection = std::make_shared<RpcConnection>(ioc_, std::move(socket), peer_id_, ssl_ctx_);

    rpc_connection->set_on_dispatch([this](auto conn, const auto &env) {
        this->dispatch_message(std::move(conn), env);
    });

    if (!initiator) {
        // Spawning in a new thread because we don't want to block here
        std::thread([this, rpc_connection, initiator]() {
            auto res = handle_connection_startup(rpc_connection, initiator);
            if (!res.has_value()) {
                Log::warn("handle_new_connection", {{"err", res.error()}}, "failed to accept incoming connection");
            }
        }).detach();
        // Add note to docs about empty string returning
        return "";
    } else {
        return handle_connection_startup(rpc_connection, initiator);
    }
}

bool RpcManager::remove_connection(const std::string &peer_id) {
    std::shared_ptr<IMessageSink> sink;
    {
        std::lock_guard<std::mutex> guard(mu_);

        auto it = connections_by_peer_.find(peer_id);
        if (it == connections_by_peer_.end()) {
            return false;
        }

        std::shared_ptr<RpcConnection> conn = it->second;

        connections_by_ip_.erase(conn->get_remote_peer_ip().ip());
        connections_by_peer_.erase(it);

        sink = sink_.lock();
    }

    if (sink) sink->on_peer_disconnected(peer_id);
    return true;
}

std::optional<std::shared_ptr<RpcConnection> > RpcManager::get_connection(const std::string &peer_id) {
    std::optional<std::shared_ptr<RpcConnection> > conn;
    {
        std::lock_guard<std::mutex> guard(mu_);
        auto it = connections_by_peer_.find(peer_id);
        if (it == connections_by_peer_.end()) {
            return nullptr;
        }
        conn = it->second;
    }

    return conn;
}

std::expected<std::future<std::string>, SendError> RpcManager::send_message(
    const std::string &peer, mesh::Envelope &envelope,
    std::optional<std::chrono::milliseconds> timeout) {
    std::shared_ptr<RpcConnection> conn;
    {
        std::lock_guard<std::mutex> guard(mu_);
        auto it = connections_by_peer_.find(peer);
        if (it == connections_by_peer_.end()) {
            return std::unexpected(SendError{INVALID_PEER});
        }
        conn = it->second;
    }

    if (timeout.has_value()) {
        return conn->send_message(envelope, timeout.value());
    } else {
        return conn->send_message(envelope);
    }
}

void RpcManager::send_heartbeats(std::chrono::milliseconds timeout) {
    // maybe change to 5 consecutive failures -> then drop the connection
    using namespace std::chrono_literals;
    std::unordered_map<std::string, int> consecutive_failed;
    while (true) {
        std::vector<std::string> peers;
        {
            std::lock_guard<std::mutex> guard(mu_);
            peers.reserve(connections_by_peer_.size());
            for (const auto &[peer_id, _]: connections_by_peer_) {
                peers.push_back(peer_id);
            }
        }

        std::unordered_map<std::string, std::future<std::string> > futures;
        for (const auto &peer: peers) {
            std::lock_guard<std::mutex> guard(mu_);
            auto it = connections_by_peer_.find(peer);
            if (it == connections_by_peer_.end())
                continue;

            std::shared_ptr<RpcConnection> conn = it->second;

            mesh::PeerIP local_ip = conn->get_local_peer_ip();
            mesh::PeerIP remote_ip = conn->get_remote_peer_ip();
            auto env = mesh::envelope::MakeHeartbeatRequest(local_ip, remote_ip);

            auto result = conn->send_message(env, timeout);
            futures.insert({peer, std::move(result)});
        }

        for (auto &pair: futures) {
            std::string peer = pair.first;
            std::future<std::string> fut = std::move(pair.second);
            try {
                fut.get();
                consecutive_failed.insert({peer, 0});
                Log::debug("heartbeat", {{"peer_id", peer}}, "successful heartbeat");
            } catch (const std::exception &e) {
                std::lock_guard<std::mutex> guard(mu_);

                int current_failures = ++consecutive_failed[peer];
                if (current_failures >= rpc::MAX_HEARTBEAT_FAILURES) {
                    Log::warn("heartbeat", {{"peer_id", peer}}, "failed too many heartbeats, removing connection");
                    connections_by_peer_.erase(peer);
                    consecutive_failed.erase(peer);
                } else {
                    Log::warn("heartbeat", {{"peer_id", peer}}, "failed heartbeat");
                }
            }
        }

        std::this_thread::sleep_for(10s);
    }
}

void RpcManager::dispatch_message(std::shared_ptr<RpcConnection> conn, const mesh::Envelope &envelope) {
    auto it = handlers_.find(envelope.type());
    if (it != handlers_.end()) {
        it->second->handle(conn, envelope);
    } else if (envelope.type() == mesh::DATA) {
        if (std::shared_ptr<IMessageSink> s = sink_.lock()) {
            s->push_data_bytes(conn->get_remote_peer_id(), envelope.payload());
        }
    }
};

void RpcManager::add_connection_internal(const std::string &peer_id, std::shared_ptr<RpcConnection> conn) {
    std::lock_guard<std::mutex> guard(mu_);
    connections_by_peer_.insert({peer_id, conn});
    connections_by_ip_.insert({conn->get_remote_peer_ip().ip(), conn});

    if (std::shared_ptr<IMessageSink> sink = sink_.lock()) {
        sink->on_peer_connected(peer_id);
    }
}

void RpcManager::add_auto_connection(const mesh::PeerIP &record) {
    std::lock_guard<std::mutex> guard(mu_);
    auto it = std::find_if(auto_connections_.begin(), auto_connections_.end(), [&](const mesh::PeerIP &r) {
        return record.ip() == r.ip() && record.port() == r.port();
    });

    if (it != auto_connections_.end()) {
        return;
    }

    auto_connections_.push_back(record);
}

void RpcManager::remove_auto_connection(const mesh::PeerIP &record) {
    std::lock_guard<std::mutex> guard(mu_);
    auto it = std::find_if(auto_connections_.begin(), auto_connections_.end(), [&](const mesh::PeerIP &r) {
        return record.ip() == r.ip() && record.port() == r.port();
    });

    if (it != auto_connections_.end()) {
        auto_connections_.erase(it);
    }
}

void RpcManager::run_maintenance_cycle() {
    maintenance_timer_.expires_after(std::chrono::seconds(5));
    maintenance_timer_.async_wait([this](const boost::system::error_code &ec) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }

        {
            std::lock_guard<std::mutex> guard(mu_);
            check_for_auto_connections_locked();
        }

        run_maintenance_cycle();
    });
}

void RpcManager::check_for_auto_connections_locked() {
    for (const auto &target_ip: auto_connections_) {
        if (connections_by_ip_.find(target_ip.ip()) != connections_by_ip_.end()) {
            continue;
        }

        Log::info("maintenance", {}, "found missing connection, attempting reconnect...");

        std::thread([this, target_ip]() {
            try {
                connect(target_ip.ip(), target_ip.port());
            } catch (const std::exception &e) {
                Log::warn("maintenance", {{"err", e.what()}}, "reconnect failed");
            }
        }).detach();
    }
}
