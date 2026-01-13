#include "RpcManager.h"

#include <expected>
#include <utility>

#include "EnvelopeUtils.h"
#include "Logger.h"
#include "handlers/HandshakeHandler.h"
#include "handlers/HeartbeatHandler.h"
#include "packet.pb.h"

RpcManager::RpcManager(boost::asio::io_context &ioc, const std::string &peer_id,
                       std::shared_ptr<IMessageSink> sink, std::shared_ptr<boost::asio::ssl::context> ssl_ctx)
    : ioc_(ioc), peer_id_(peer_id), sink_(sink), ssl_ctx_(std::move(ssl_ctx)) {
    register_handlers();
    using namespace std::chrono_literals;
    heartbeat_thread_ = std::thread([this] { send_heartbeats(300ms); });
}

void RpcManager::register_handlers() {
    handlers_[mesh::EnvelopeType::HANDSHAKE] = std::make_unique<HandshakeHandler>();
    handlers_[mesh::EnvelopeType::HEARTBEAT] = std::make_unique<HeartbeatHandler>();
}

RpcManager::~RpcManager() = default;

void RpcManager::set_sink(const std::shared_ptr<IMessageSink> &sink) {
    sink_ = sink;
}

std::expected<std::string, std::string> RpcManager::create_connection(const std::string &remote_addr,
                                                                      boost::asio::ip::tcp::socket sock) {
    boost::system::error_code ec;
    auto remote_ep = sock.remote_endpoint(ec);
    if (ec) {
        Log::warn("create_connection",
                  {{"remote_address", remote_addr}},
                  "failed to create remote endpoint"
        );
        return std::unexpected("failed to create connection");
    }
    auto local_endpoint = sock.local_endpoint();
    auto rpc_connection = std::make_shared<RpcConnection>(ioc_, std::move(sock), peer_id_, local_endpoint,
                                                          remote_ep, ssl_ctx_);
    rpc_connection->set_on_dispatch([this](auto conn, const auto &env) {
        this->dispatch_message(conn, env);
    });

    // Safe to block the main thread while waiting for a handshake
    auto res = rpc_connection->start(true); // Blocking call
    if (res.has_value()) {
        std::lock_guard<std::mutex> guard(mu_);
        auto remote_peer_id = res.value().peer_id();
        rpc_connection->set_remote_peer_id(remote_peer_id);
        connections_.insert({remote_peer_id, rpc_connection});

        if (std::shared_ptr<IMessageSink> sink = sink_.lock()) {
            sink->on_peer_connected(remote_peer_id);
        }

        return remote_peer_id;
    } else {
        return std::unexpected(res.error());
    }
}

void RpcManager::accept_connection(const std::string &remote_addr, boost::asio::ip::tcp::socket sock) {
    auto remote_ep = sock.remote_endpoint();
    auto local_ep = sock.local_endpoint();

    auto rpc_connection = std::make_shared<RpcConnection>(ioc_, std::move(sock), peer_id_, local_ep, remote_ep,
                                                          ssl_ctx_);
    rpc_connection->set_on_dispatch([this](auto conn, const auto &env) {
        this->dispatch_message(conn, env);
    });

    // Spawning in a new thread because we don't want to block here
    std::thread([this, rpc_connection]() {
        auto res = rpc_connection->start(false); // Blocking call

        if (res.has_value()) {
            const std::string &peer_id = res.value().peer_id();
            Log::info("accept_connection", {{"peer_id", peer_id}}, "handshake complete, accepted peer");
            std::lock_guard<std::mutex> guard(mu_);
            auto remote_peer_id = peer_id;
            rpc_connection->set_remote_peer_id(remote_peer_id);
            connections_.insert({remote_peer_id, rpc_connection});

            if (std::shared_ptr<IMessageSink> sink = sink_.lock()) {
                sink->on_peer_connected(remote_peer_id);
            }
        } else {
            Log::warn("accept_connection", {{"err", res.error()}}, "handshake failed");
        }
    }).detach();
}

bool RpcManager::remove_connection(std::string peer_id) {
    std::shared_ptr<IMessageSink> sink;
    {
        std::lock_guard<std::mutex> guard(mu_);
        connections_.erase(peer_id);
        sink = sink_.lock();
    }

    if (sink) sink->on_peer_disconnected(peer_id);
    return true;
}

std::optional<std::shared_ptr<RpcConnection> > RpcManager::get_connection(const std::string &peer_id) {
    std::optional<std::shared_ptr<RpcConnection> > conn;
    {
        std::lock_guard<std::mutex> guard(mu_);
        auto it = connections_.find(peer_id);
        if (it == connections_.end()) {
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
        auto it = connections_.find(peer);
        if (it == connections_.end()) {
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
            peers.reserve(connections_.size());
            for (const auto &[peer_id, _]: connections_) {
                peers.push_back(peer_id);
            }
        }

        std::unordered_map<std::string, std::future<std::string> > futures;
        for (const auto &peer: peers) {
            std::lock_guard<std::mutex> guard(mu_);
            auto it = connections_.find(peer);
            if (it == connections_.end())
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
                    connections_.erase(peer);
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
    } else {
        if (std::shared_ptr<IMessageSink> s = sink_.lock()) {
            s->push_data_bytes(conn->get_remote_peer_id(), envelope.payload());
        }
    }
};
