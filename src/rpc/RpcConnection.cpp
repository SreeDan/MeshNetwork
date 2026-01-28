#include "RpcConnection.h"

#include <expected>
#include <iostream>
#include <utility>
#include <boost/asio/steady_timer.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "EnvelopeUtils.h"
#include "Logger.h"

using namespace std::chrono_literals;

RpcConnection::RpcConnection(boost::asio::io_context &ioc,
                             boost::asio::ip::tcp::socket sock,
                             const std::string &peer_id,
                             int udp_port,
                             const std::shared_ptr<boost::asio::ssl::context> &ssl_ctx)
    : ioc_(ioc),
      sock_(std::move(sock)),
      peer_id_(peer_id),
      my_udp_port_(udp_port),
      local_endpoint_(sock.local_endpoint()),
      remote_endpoint_(sock.remote_endpoint()),
      ssl_ctx_(ssl_ctx) {
}

RpcConnection::~RpcConnection() {
    std::lock_guard<std::mutex> guard(mu_);
    for (auto &[req_id, pending_req]: pending_requests_) {
        if (pending_req.timer) {
            pending_req.timer->cancel();
        }

        try {
            pending_req.prom.set_exception(
                std::make_exception_ptr(std::runtime_error("RpcLayer destroyed")));
        } catch (...) {
        }
    }
    pending_requests_.clear();
}

std::expected<mesh::PeerRecord, std::string> RpcConnection::start(bool initiator) {
    auto self = shared_from_this();
    handshake_promise_ = std::make_shared<std::promise<std::expected<mesh::PeerRecord, std::string> > >();
    if (ssl_ctx_ == nullptr) {
        session_ = make_tcp_session(ioc_, std::move(sock_),
                                    [self](const boost::uuids::uuid &msg_id, const std::string &payload) {
                                        self->on_message(msg_id, payload);
                                    });
    } else {
        session_ = make_ssl_session(ioc_, std::move(sock_), *ssl_ctx_, initiator,
                                    [self](const boost::uuids::uuid &msg_id, const std::string &payload) {
                                        self->on_message(msg_id, payload);
                                    });
    }
    session_->start();

    if (initiator) {
        // If we are the initiator, we actively send and wait for a reply
        return send_handshake_request();
    } else {
        // If we are the acceptor, we simply wait for the remote to send us a handshake
        // This .get() blocks this thread until on_message sets the promise.
        return handshake_promise_->get_future().get();
    }
}


void RpcConnection::set_on_dispatch(std::function<void(std::shared_ptr<RpcConnection>, const mesh::Envelope &)> cb) {
    handler_cb_ = cb;
}

mesh::PeerIP RpcConnection::get_local_peer_ip() {
    mesh::PeerIP local_ip;
    local_ip.set_ip(local_endpoint_.address().to_string());
    local_ip.set_tcp_port(local_endpoint_.port());
    local_ip.set_udp_port(my_udp_port_);
    return local_ip;
}

mesh::PeerIP RpcConnection::get_remote_peer_ip() {
    mesh::PeerIP remote_ip;
    remote_ip.set_ip(remote_endpoint_.address().to_string());
    remote_ip.set_tcp_port(remote_endpoint_.port());
    remote_ip.set_udp_port(remote_udp_endpoint_.port()); // Defaults to 0 if not set
    return remote_ip;
}

std::string RpcConnection::get_self_peer_id() {
    return peer_id_;
}

std::string RpcConnection::get_remote_peer_id() {
    return authenticated_remote_id_;
}

void RpcConnection::set_remote_peer_id(const std::string &peer_id) {
    authenticated_remote_id_ = peer_id;
}

std::expected<mesh::PeerRecord, std::string> RpcConnection::send_handshake_request() {
    mesh::PeerIP local_ip = get_local_peer_ip();
    mesh::PeerIP remote_ip = get_remote_peer_ip();

    auto env = mesh::envelope::MakeHandshakeRequest(local_ip, remote_ip, peer_id_);
    std::future<std::string> fut_response = send_message(env);

    try {
        std::string response = fut_response.get();

        mesh::Envelope response_envelope;
        if (!response_envelope.ParseFromString(response))
            return std::unexpected("failed to parse handshake response envelope");
        if (response_envelope.type() != mesh::EnvelopeType::HANDSHAKE)
            return std::unexpected("invalid handshake response type");

        mesh::Handshake handshake_response;
        if (!handshake_response.ParseFromString(response_envelope.payload()))
            return std::unexpected("failed to parse handshake payload");

        auto remote_address = remote_endpoint_.address();
        remote_udp_endpoint_ = boost::asio::ip::udp::endpoint(
            remote_address, handshake_response.peer_record().peer_ip().udp_port());

        return handshake_response.peer_record();
    } catch (std::exception &e) {
        return std::unexpected(std::string("Handshake error: ") + e.what());
    }
}

std::future<std::string> RpcConnection::send_message(
    mesh::Envelope &envelope,
    std::chrono::milliseconds timeout) {
    *envelope.mutable_from() = get_local_peer_ip();
    *envelope.mutable_to() = get_remote_peer_ip();

    std::string message_contents = envelope.SerializeAsString();
    std::string req_id = envelope.msg_id();
    // format is 2 bytes of the len(req_id) + req_id + wrapped_req

    std::promise<std::string> promise;
    auto fut = promise.get_future();

    auto timer = std::make_unique<boost::asio::steady_timer>(ioc_, timeout);
    auto time_ptr = timer.get();

    if (envelope.expect_response()) {
        // Only put the promise in pending_requests_ if we expect a response
        // because if we don't expect a response we won't ever get a response.
        std::lock_guard<std::mutex> guard(mu_);
        pending_requests_[req_id] = PendingRequest{
            std::move(promise),
            std::move(timer)
        };

        // timer expiration logic
        time_ptr->async_wait([this, req_id](const boost::system::error_code &ec) {
            if (ec == boost::asio::error::operation_aborted) {
                // timer was canceled, meaning the response did not time out
                return;
            }

            std::lock_guard<std::mutex> guard(mu_);
            auto it = pending_requests_.find(req_id);
            if (it == pending_requests_.end()) {
                return;
            }

            try {
                it->second.prom.set_exception(
                    // TODO: Make it a known exception I can explicitly handle
                    std::make_exception_ptr(std::runtime_error("RPC failed: timeout")));
            } catch (...) {
            }
            pending_requests_.erase(it);
        });
    } else {
        promise.set_value("");
    }

    mesh::PeerIP self_peer = get_local_peer_ip();
    if (self_peer.ip() == envelope.to().ip() && self_peer.tcp_port() == envelope.to().tcp_port()) {
        Log::warn("rpc:send_message", {{"message_id", envelope.msg_id()}}, "sending message to itself");
    }

    session_->async_send_message(req_id, message_contents);
    return fut;
}

void RpcConnection::on_message(const boost::uuids::uuid &msg_id, const std::string &payload) {
    mesh::Envelope env;
    if (!env.ParseFromString(payload)) {
        std::cerr << "Failed to parse envelope\n";
        return;
    }

    store_response(msg_id, payload);
    if (handler_cb_) {
        // Incoming request from peer
        handler_cb_(shared_from_this(), env);
    }
}

void RpcConnection::store_response(const boost::uuids::uuid &msg_id, const std::string &response) {
    std::lock_guard<std::mutex> guard(mu_);

    std::string binary_uuid(reinterpret_cast<const char *>(msg_id.data()), 16);
    if (pending_requests_.contains(binary_uuid)) {
        auto it = pending_requests_.find(binary_uuid);
        try {
            it->second.prom.set_value(response);
        } catch (...) {
            std::cerr << "receive response failed for request " << boost::uuids::to_string(msg_id) << std::endl;
        }
        pending_requests_.erase(it);
    }
}

void RpcConnection::fulfill_handshake_promise(const mesh::PeerRecord &record) {
    if (handshake_promise_) {
        try {
            handshake_promise_->set_value(record);
        } catch (std::exception &e) {
            std::cerr << "failed to set handshake promise value: " << e.what() << std::endl;
        }
    }
}
