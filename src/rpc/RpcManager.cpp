#include "RpcManager.h"

#include <expected>

#include "EnvelopeUtils.h"

RpcManager::RpcManager(boost::asio::io_context &ioc, const std::string &peer_id)
    : ioc_(ioc), peer_id_(peer_id) {
    using namespace std::chrono_literals;
    heartbeat_thread_ = std::thread([this] { send_heartbeats(300ms); });
}

RpcManager::~RpcManager() {
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
    const std::string &peer, mesh::Envelope envelope,
    std::optional<std::chrono::milliseconds> timeout) {
    std::lock_guard<std::mutex> guard(mu_);
    auto it = connections_.find(peer);
    if (it == connections_.end()) {
        return std::unexpected(SendError{INVALID_PEER});
    }

    std::shared_ptr<RpcConnection> conn = it->second;
    if (timeout.has_value()) {
        return conn->send_request(envelope, timeout.value());
    } else {
        return conn->send_request(envelope);
    }
}

void RpcManager::send_heartbeats(std::chrono::milliseconds timeout) {
    using namespace std::chrono_literals;
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

            auto result = conn->send_request(env, timeout);
            futures.insert({peer, std::move(result)});
        }

        for (auto &pair: futures) {
            std::string peer = pair.first;
            std::future<std::string> fut = std::move(pair.second);
            try {
                fut.get();
                std::cout << "successful heartbeat with " << peer << std::endl;
            } catch (const std::exception &e) {
                std::cerr << e.what() << std::endl;
                std::lock_guard<std::mutex> guard(mu_);
                connections_.erase(peer);
                std::cout << "failed heartbeat with " << peer << ", removing connection" << std::endl;
            }
        }

        std::this_thread::sleep_for(3s);
    }
}
