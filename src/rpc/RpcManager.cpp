#include "RpcManager.h"

#include <expected>

#include "EnvelopeUtils.h"

RpcManager::RpcManager(boost::asio::io_context &ioc, const std::string &peer_id)
    : ioc_(ioc), peer_id_(peer_id) {
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
    rpc_connection->start();

    mesh::PeerIP local_ip;
    local_ip.set_ip(local_endpoint.address().to_string());
    local_ip.set_port(local_endpoint.port());

    mesh::PeerIP remote_ip;
    remote_ip.set_ip(remote_ep.address().to_string());
    remote_ip.set_port(remote_ep.port());

    auto env = mesh::envelope::MakeHandshakeRequest(local_ip, remote_ip, peer_id_);
    std::cout << env.SerializeAsString().size() << std::endl;
    std::future<std::string> fut_response = rpc_connection->send_request(env);
    std::string response = fut_response.get();

    mesh::Envelope response_envelope;
    if (!response_envelope.ParseFromString(response))
        return std::unexpected("failed to parse handshake response envelope");

    // Not the correct response
    if (response_envelope.type() != mesh::EnvelopeType::HANDSHAKE || response_envelope.expect_response() == true)
        return std::unexpected("invalid handshake response: " + response);

    mesh::Handshake handshake_response;
    if (!handshake_response.ParseFromString(response_envelope.payload())) {
        return std::unexpected("failed to parse handshake in response");
    }

    if (handshake_response.proto_version() != mesh::MeshVersion) {
        std::ostringstream oss;
        oss << "mesh protobuf versions don't match, expected: "
                << mesh::MeshVersion << ", received: " << handshake_response.proto_version();
        return std::unexpected(oss.str());
    }

    // Sanity check ip address and port
    mesh::PeerRecord rec = handshake_response.peer_record();
    if (rec.peer_ip().ip() != remote_ip.ip() || rec.peer_ip().port() != remote_ep.port()) {
        return std::unexpected("IP response of remote is different than expected");
    }

    connections_.insert({rec.peer_id(), rpc_connection});
    return rec.peer_id();
}

void RpcManager::accept_connection(const std::string &remote_addr, boost::asio::ip::tcp::socket sock) {
    boost::system::error_code ec;
    auto remote_ep = sock.remote_endpoint(ec);
    if (ec) {
        std::cerr << "remote_endpoint failed: " << ec.message() << std::endl;
        return;
    }
    auto local_endpoint = sock.local_endpoint();
    auto rpc_connection = std::make_shared<RpcConnection>(ioc_, std::move(sock), peer_id_, local_endpoint,
                                                          remote_ep);
    connections_.insert({remote_addr, rpc_connection});
    rpc_connection->start();
}

bool RpcManager::remove_connection(std::string peer_id) {
    return connections_.erase(peer_id);
}

std::optional<std::shared_ptr<RpcConnection> > RpcManager::get_connection(const std::string &peer_id) {
    auto it = connections_.find(peer_id);
    if (it == connections_.end()) {
        return nullptr;
    }

    return it->second;
}

std::expected<std::future<std::string>, SendError> RpcManager::send_message(
    const std::string &peer, mesh::Envelope envelope,
    std::optional<std::chrono::milliseconds> timeout) {
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

