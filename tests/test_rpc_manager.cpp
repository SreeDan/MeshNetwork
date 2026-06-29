#include <chrono>
#include <expected>
#include <future>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mesh/rpc/RpcManager.h"

namespace {

bool run_for(boost::asio::io_context &ioc, std::chrono::milliseconds timeout,
             const std::function<bool()> &done) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        ioc.run_for(std::chrono::milliseconds(5));
        ioc.restart();
    }
    return done();
}

boost::asio::ip::tcp::acceptor make_dummy_acceptor(boost::asio::io_context &ioc,
                                                   boost::asio::ip::tcp::socket &accepted_socket) {
    boost::asio::ip::tcp::acceptor acceptor(ioc, {boost::asio::ip::address_v4::loopback(), 0});
    acceptor.async_accept(accepted_socket, [](boost::system::error_code ec) {
        REQUIRE_FALSE(ec);
    });
    return acceptor;
}

}

TEST_CASE("RpcManager connect_async times out when TCP peer never handshakes", "[rpc][connect]") {
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::socket accepted_socket(ioc);
    auto acceptor = make_dummy_acceptor(ioc, accepted_socket);
    RpcManager manager(ioc, "nodeA", 0, 0);

    bool completed = false;
    std::expected<std::string, std::string> result;
    manager.connect_async(
        "127.0.0.1",
        acceptor.local_endpoint().port(),
        std::chrono::milliseconds(200),
        [&](std::expected<std::string, std::string> res) {
            result = std::move(res);
            completed = true;
        });

    REQUIRE(run_for(ioc, std::chrono::seconds(1), [&] { return completed; }));
    REQUIRE_FALSE(result.has_value());
    const bool timeout_error = result.error().find("timed out") != std::string::npos ||
                               result.error().find("timeout") != std::string::npos;
    REQUIRE(timeout_error);

    manager.shutdown();
}

TEST_CASE("RpcManager connect_async suppresses duplicate pending attempts", "[rpc][connect]") {
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::socket accepted_socket(ioc);
    auto acceptor = make_dummy_acceptor(ioc, accepted_socket);
    RpcManager manager(ioc, "nodeA", 0, 0);

    bool first_completed = false;
    bool second_completed = false;
    std::expected<std::string, std::string> second_result;

    manager.connect_async(
        "127.0.0.1",
        acceptor.local_endpoint().port(),
        std::chrono::milliseconds(200),
        [&](std::expected<std::string, std::string>) {
            first_completed = true;
        });

    manager.connect_async(
        "127.0.0.1",
        acceptor.local_endpoint().port(),
        std::chrono::milliseconds(200),
        [&](std::expected<std::string, std::string> res) {
            second_result = std::move(res);
            second_completed = true;
        });

    REQUIRE(second_completed);
    REQUIRE_FALSE(second_result.has_value());
    REQUIRE(second_result.error().find("pending") != std::string::npos);
    REQUIRE(run_for(ioc, std::chrono::seconds(1), [&] { return first_completed; }));

    manager.shutdown();
}

TEST_CASE("RpcManager connect blocking wrapper returns async timeout failure", "[rpc][connect]") {
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::socket accepted_socket(ioc);
    auto acceptor = make_dummy_acceptor(ioc, accepted_socket);
    RpcManager manager(ioc, "nodeA", 0, 0);

    std::thread io_thread([&] {
        ioc.run();
    });

    auto result = manager.connect(
        "127.0.0.1",
        acceptor.local_endpoint().port(),
        std::chrono::milliseconds(200));

    REQUIRE_FALSE(result.has_value());
    const bool timeout_error = result.error().find("timed out") != std::string::npos ||
                               result.error().find("timeout") != std::string::npos;
    REQUIRE(timeout_error);

    manager.shutdown();
    ioc.stop();
    io_thread.join();
}
