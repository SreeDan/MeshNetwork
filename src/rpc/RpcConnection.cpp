#include "RpcConnection.h"

#include <iostream>
#include <utility>
#include <boost/asio/steady_timer.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
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

// RpcConnection::RpcConnection(RpcConnection &&other) noexcept
//     : ioc_(other.ioc_), sock_(std::move(other.sock_)),
//       pending_requests_(std::move(other.pending_requests_)) {
// }
//
// RpcConnection &RpcConnection::operator=(RpcConnection &&other) noexcept {
//     if (this == &other) {
//         return *this;
//     }
//
//     for (auto &[req_id, pending_req]: pending_requests_) {
//         if (pending_req.timer) {
//             boost::system::error_code ec;
//             pending_req.timer->cancel(ec);
//         }
//
//         try {
//             pending_req.prom.set_exception(
//                 std::make_exception_ptr(std::runtime_error("RpcLayer moved")));
//         } catch (...) {
//         }
//     }
//
//     {
//         // Move from other
//         std::lock_guard<std::mutex> g(other.mu_);
//         pending_requests_ = std::move(other.pending_requests_);
//         sock_ = std::move(other.sock_);
//     }
//
//     return *this;
// }

void RpcConnection::start() {
    auto self = shared_from_this();
    session_ = make_plain_session(ioc_, std::move(sock_),
                                  [self](const boost::uuids::uuid &msg_id, const std::string &payload) {
                                      self->on_message(msg_id, payload);
                                  });
    std::cout << "starting session layer\n";
    // TODO: Eventually handle constructing a TLS session
    session_->start();
}


std::future<std::string> RpcConnection::send_request(
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
        {
            std::lock_guard<std::mutex> guard(mu_);
            pending_requests_[req_id] = Pending{
                std::move(promise),
                std::move(timer)
            };
        }
    }

    // timer expiration logic
    time_ptr->async_wait([this, req_id](const boost::system::error_code &ec) {
        if (ec == boost::asio::error::operation_aborted) {
            // timer was cancelled, meaning the response did not time out
            return;
        }

        std::lock_guard<std::mutex> guard(mu_);
        auto it = pending_requests_.find(req_id);
        if (it == pending_requests_.end()) {
            return;
        }

        try {
            it->second.prom.set_exception(
                std::make_exception_ptr(std::runtime_error("RPC failed: timeout")));
        } catch (...) {
        }
        pending_requests_.erase(it);
    });

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

    if (env.expect_response()) {
        // Incoming request from peer
        respond_to_request(msg_id, env);
    } else {
        // Incoming response to our earlier request
        store_response(msg_id, payload);
    }
}

void RpcConnection::respond_to_request(const boost::uuids::uuid &msg_id, const mesh::Envelope &env) {
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

        // Construct handshake response
        auto response = mesh::envelope::MakeHandshakeResponse(local_ip, env.from(), peer_id_);
        response.set_msg_id(env.msg_id());

        std::string req_id = response.msg_id();
        // Send response
        session_->async_send_message(req_id, response.SerializeAsString());
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
