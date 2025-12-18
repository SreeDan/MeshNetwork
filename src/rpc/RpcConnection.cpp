#include "RpcConnection.h"

#include <expected>
#include <iostream>
#include <utility>
#include <boost/asio/steady_timer.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "EnvelopeUtils.h"

using namespace std::chrono_literals;

RpcConnection::RpcConnection(boost::asio::io_context &ioc, boost::asio::ip::tcp::socket sock,
                             const std::string &peer_id, const boost::asio::ip::tcp::endpoint &local_ep,
                             const boost::asio::ip::tcp::endpoint &remote_ep)
    : ioc_(ioc), sock_(std::move(sock)), peer_id_(peer_id), local_endpoint_(local_ep), remote_endpoint_(remote_ep) {
}

RpcConnection::~RpcConnection() {
    std::lock_guard<std::mutex> guard(mu_);
    for (auto &[req_id, pending_req]: pending_requests_) {
        if (pending_req.timer) {
            boost::system::error_code ec;
            pending_req.timer->cancel(ec);
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
    session_ = make_plain_session(ioc_, std::move(sock_),
                                  [self](const boost::uuids::uuid &msg_id, const std::string &payload) {
                                      self->on_message(msg_id, payload);
                                  });
    std::cout << "starting session layer\n";
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
    local_ip.set_port(local_endpoint_.port());
    return local_ip;
}

mesh::PeerIP RpcConnection::get_remote_peer_ip() {
    mesh::PeerIP remote_ip;
    remote_ip.set_ip(remote_endpoint_.address().to_string());
    remote_ip.set_port(remote_endpoint_.port());
    return remote_ip;
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

        return handshake_response.peer_record();
    } catch (std::exception &e) {
        return std::unexpected(std::string("Handshake error: ") + e.what());
    }
}

std::future<std::string> RpcConnection::send_message(
    const mesh::Envelope &envelope,
    std::chrono::milliseconds timeout) {
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
        pending_requests_[req_id] = Pending{
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
    if (self_peer.ip() == envelope.to().ip() && self_peer.port() == envelope.to().port()) {
        std::cout << "warning: sending message to itself" << std::endl;
    }

    session_->async_send_message(req_id, message_contents);
    return fut;
}

void RpcConnection::on_message(const boost::uuids::uuid &msg_id, const std::string &payload) {
    std::cout << "[debug] payload size = " << payload.size() << std::endl;
    mesh::Envelope env;
    if (!env.ParseFromString(payload)) {
        std::cerr << "Failed to parse envelope\n";
        return;
    }

    std::cout << "on message reached: " << payload << std::endl;

    store_response(msg_id, payload);
    if (handler_cb_) {
        std::cout << "calling dispatcher" << std::endl;
        // Incoming request from peer
        handler_cb_(shared_from_this(), env);
    }
}

void RpcConnection::respond_to_message(const boost::uuids::uuid &msg_id, const mesh::Envelope &env) {
    if (env.type() == mesh::EnvelopeType::HANDSHAKE && env.expect_response()) {
        std::cout << "performing response stuff\n";
        // Deserialize the request
        mesh::Handshake handshake_req;
        if (!handshake_req.ParseFromString(env.payload())) {
            std::cerr << "Failed to parse handshake request\n";
            return;
        }

        mesh::PeerIP local_ip;
        local_ip.set_ip(local_endpoint_.address().to_string());
        local_ip.set_port(local_endpoint_.port());

        auto response = mesh::envelope::MakeHandshakeResponse(local_ip, env.from(), peer_id_);
        response.set_msg_id(env.msg_id());

        session_->async_send_message(response.msg_id(), response.SerializeAsString());

        if (handshake_promise_) {
            try {
                handshake_promise_->set_value(handshake_req.peer_record());
            } catch (...) {
            }
        }
    } else if (env.type() == mesh::EnvelopeType::HEARTBEAT && env.expect_response()) {
        mesh::PeerIP local_ip;
        local_ip.set_ip(local_endpoint_.address().to_string());
        local_ip.set_port(local_endpoint_.port());

        auto response = mesh::envelope::MakeHeartbeatResponse(local_ip, env.from());
        response.set_msg_id(env.msg_id());

        session_->async_send_message(response.msg_id(), response.SerializeAsString());
    }
}

void RpcConnection::store_response(const boost::uuids::uuid &msg_id, const std::string &response) {
    std::lock_guard<std::mutex> guard(mu_);

    for (const auto &pair: pending_requests_) {
        std::cout << pair.first << std::endl;
    }

    std::string binary_uuid(reinterpret_cast<const char *>(msg_id.data), 16);
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
