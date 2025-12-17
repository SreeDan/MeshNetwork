#pragma once
#include <expected>
#include <future>
#include <string>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/local/basic_endpoint.hpp>

#include "session.h"

using namespace std::chrono_literals;

struct Pending {
    std::promise<std::string> prom;
    std::unique_ptr<boost::asio::steady_timer> timer;
};

class RpcConnection : public std::enable_shared_from_this<RpcConnection> {
public:
    RpcConnection(boost::asio::io_context &ioc, boost::asio::ip::tcp::socket sock, const std::string &peer_id,
                  const boost::asio::ip::tcp::endpoint &local_ep, const boost::asio::ip::tcp::endpoint &remote_ep);

    ~RpcConnection();

    RpcConnection(RpcConnection &&other) = delete;

    RpcConnection &operator=(RpcConnection &&other) = delete;

    RpcConnection(const RpcConnection &other) = delete;

    RpcConnection &operator=(const RpcConnection &other) = delete;

    std::expected<mesh::PeerRecord, std::string> start(bool initiator);

    std::expected<mesh::PeerRecord, std::string> send_handshake_request();

    std::expected<mesh::PeerRecord, std::string> send_handshake_request2();

    std::future<std::string> send_request(
        const mesh::Envelope &envelope,
        std::chrono::milliseconds timeout = 3000ms
    );

    boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> local_endpoint_;
    boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> remote_endpoint_;
    const std::string &peer_id_;

private:
    boost::asio::io_context &ioc_;
    boost::asio::ip::tcp::socket sock_;

    std::shared_ptr<std::promise<std::expected<mesh::PeerRecord, std::string> > > handshake_promise_;

    // Guards pending_requests_
    std::mutex mu_;
    std::unordered_map<std::string, Pending> pending_requests_;
    std::shared_ptr<ISession> session_;

    void on_message(const boost::uuids::uuid &msg_id, const std::string &payload);

    void respond_to_request(const boost::uuids::uuid &msg_id, const mesh::Envelope &env);

    void store_response(const boost::uuids::uuid &msg_id, const std::string &resp_payload);
};

