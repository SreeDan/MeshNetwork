#include "RpcManager.h"

#include <expected>

#include "EnvelopeUtils.h"
#include "handlers/HandshakeHandler.h"
#include "handlers/HeartbeatHandler.h"
#include "handlers/TextHandler.h"
#include "packet.pb.h"

RpcManager::RpcManager(boost::asio::io_context &ioc, const std::string &peer_id,
                       std::shared_ptr<IMessageSink> sink)
    : ioc_(ioc), peer_id_(peer_id), sink_(sink) {
    register_handlers();
    using namespace std::chrono_literals;
    heartbeat_thread_ = std::thread([this] { send_heartbeats(300ms); });
}

void RpcManager::register_handlers() {
    handlers_[mesh::EnvelopeType::HANDSHAKE] = std::make_unique<HandshakeHandler>();
    handlers_[mesh::EnvelopeType::HEARTBEAT] = std::make_unique<HeartbeatHandler>();
    // handlers_[mesh::EnvelopeType::DATA] = std::make_unique<TextHandler>();
}

RpcManager::~RpcManager() {
}

void RpcManager::set_sink(std::shared_ptr<IMessageSink> sink) {
    sink_ = sink;
}

std::expected<std::string, std::string> RpcManager::create_connection(const std::string &remote_addr,
                                                                      boost::asio::ip::tcp::socket sock) {
    boost::system::error_code ec;
    auto remote_ep = sock.remote_endpoint(ec);
    if (ec) {
        std::cerr << "remote_endpoint failed: " << ec.message() << std::endl;
        return std::unexpected("fail");
    }
    auto local_endpoint = sock.local_endpoint();
    auto rpc_connection = std::make_shared<RpcConnection>(ioc_, std::move(sock), peer_id_, local_endpoint,
                                                          remote_ep);
    rpc_connection->set_on_dispatch([this](auto conn, const auto &env) {
        this->dispatch_message(conn, env);
    });

    // Safe to block the main thread while waiting for a handshake
    auto res = rpc_connection->start(true); // Blocking call
    if (res.has_value()) {
        std::lock_guard<std::mutex> guard(mu_);
        connections_.insert({res.value().peer_id(), rpc_connection});
        return res.value().peer_id();
    } else {
        return std::unexpected(res.error());
    }
}

void RpcManager::accept_connection(const std::string &remote_addr, boost::asio::ip::tcp::socket sock) {
    auto remote_ep = sock.remote_endpoint();
    auto local_ep = sock.local_endpoint();

    auto rpc_connection = std::make_shared<RpcConnection>(ioc_, std::move(sock), peer_id_, local_ep, remote_ep);
    rpc_connection->set_on_dispatch([this](auto conn, const auto &env) {
        this->dispatch_message(conn, env);
    });

    // Spawning in a new thread because we don't want to block here
    std::thread([this, rpc_connection]() {
        std::cout << "starting new std::thread for accepting connection" << std::endl;
        auto res = rpc_connection->start(false); // Blocking call

        if (res.has_value()) {
            std::cout << "Handshake complete. Accepted peer: " << res.value().peer_id() << std::endl;
            std::lock_guard<std::mutex> guard(mu_);
            connections_.insert({res.value().peer_id(), rpc_connection});
        } else {
            std::cerr << "Handshake failed: " << res.error() << std::endl;
        }
    }).detach();
}

bool RpcManager::remove_connection(std::string peer_id) {
    std::lock_guard<std::mutex> guard(mu_);
    return connections_.erase(peer_id);
}

std::optional<std::shared_ptr<RpcConnection> > RpcManager::get_connection(const std::string &peer_id) {
    std::lock_guard<std::mutex> guard(mu_);
    auto it = connections_.find(peer_id);
    if (it == connections_.end()) {
        return nullptr;
    }

    return it->second;
}

std::expected<std::future<std::string>, SendError> RpcManager::send_message(
    const std::string &peer, mesh::Envelope &envelope,
    std::optional<std::chrono::milliseconds> timeout) {
    std::lock_guard<std::mutex> guard(mu_);
    auto it = connections_.find(peer);
    if (it == connections_.end()) {
        return std::unexpected(SendError{INVALID_PEER});
    }

    std::shared_ptr<RpcConnection> conn = it->second;
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

            mesh::PeerIP local_ip;
            local_ip.set_ip(conn->local_endpoint_.address().to_string());
            local_ip.set_port(conn->local_endpoint_.port());

            mesh::PeerIP remote_ip;
            remote_ip.set_ip(conn->remote_endpoint_.address().to_string());
            remote_ip.set_port(conn->remote_endpoint_.port());

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
                std::cout << "successful heartbeat with " << peer << std::endl;
            } catch (const std::exception &e) {
                std::cerr << e.what() << std::endl;
                std::lock_guard<std::mutex> guard(mu_);

                int current_failures = ++consecutive_failed[peer];
                if (current_failures >= rpc::MAX_HEARTBEAT_FAILURES) {
                    connections_.erase(peer);
                    consecutive_failed.erase(peer);
                    std::cout << "failed heartbeat with " << peer << ", removing connection" << std::endl;
                } else {
                    std::cout << "failed heartbeat with " << peer << std::endl;
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
    }

    if (std::shared_ptr<IMessageSink> s = sink_.lock()) {
        s->push_packet(conn->peer_id_.data(), envelope);
    }
};
