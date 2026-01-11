#pragma once
#include <expected>
#include <future>
#include <string>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include "envelope.pb.h"

#include "session.h"

using namespace std::chrono_literals;

struct PendingRequest {
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

    std::future<std::string> send_message(
        mesh::Envelope &envelope,
        std::chrono::milliseconds timeout = 3000ms
    );

    mesh::PeerIP get_local_peer_ip();

    mesh::PeerIP get_remote_peer_ip();

    void set_on_dispatch(std::function<void(std::shared_ptr<RpcConnection>, const mesh::Envelope &)> cb);

    void fulfill_handshake_promise(const mesh::PeerRecord &record);

    std::string get_self_peer_id();

    void set_remote_peer_id(const std::string &peer_id);

    std::string get_remote_peer_id();

    boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> local_endpoint_;
    boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> remote_endpoint_;

private:
    boost::asio::io_context &ioc_;
    boost::asio::ip::tcp::socket sock_;

    const std::string peer_id_;
    std::string authenticated_remote_id_;

    std::shared_ptr<std::promise<std::expected<mesh::PeerRecord, std::string> > > handshake_promise_;

    // Guards pending_requests_
    std::mutex mu_;
    std::unordered_map<std::string, PendingRequest> pending_requests_;
    std::shared_ptr<ISession> session_;
    std::function<void(std::shared_ptr<RpcConnection>, const mesh::Envelope &)> handler_cb_;

    void on_message(const boost::uuids::uuid &msg_id, const std::string &payload);

    void respond_to_message(const boost::uuids::uuid &msg_id, const mesh::Envelope &env);

    void store_response(const boost::uuids::uuid &msg_id, const std::string &resp_payload);
};

