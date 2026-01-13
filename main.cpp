#include <filesystem>
#include <iostream>
#include <thread>
#include <boost/asio.hpp>

#include "Logger.h"
#include "mesh_node.h"

std::shared_ptr<boost::asio::ssl::context> make_ssl_context(
    const std::string &cert_file,
    const std::string &key_file,
    const std::string &ca_file
) {
    try {
        std::shared_ptr<boost::asio::ssl::context> ssl_ctx;

        // initialize context for TLS v1.2 or v1.3
        ssl_ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);

        ssl_ctx->set_options(
            boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::single_dh_use
        );

        // Load certificates from the file paths
        ssl_ctx->use_certificate_chain_file(cert_file);
        ssl_ctx->use_private_key_file(key_file, boost::asio::ssl::context::pem);

        // If you need to verify peers (mutual TLS), load the CA
        if (!ca_file.empty()) {
            ssl_ctx->load_verify_file(ca_file);
            ssl_ctx->set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert);
        }

        std::cout << "TLS Context initialized successfully." << std::endl;

        return ssl_ctx;
    } catch (const std::exception &e) {
        throw std::runtime_error("TLS Setup Failed: " + std::string(e.what()));
    }
}

// TODO: Make arguments come from a yml file instead
bool has_arg(int argc, char *argv[], const std::string &target) {
    for (int i = 0; i < argc; ++i) {
        if (argv[i] == target) {
            return true;
        }
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "Usage: meshnode <tcp_port> <udp_discovery_port> <peer_id> [--tls cert key ca]\n";
        return 1;
    }
    int tcp_port = std::stoi(argv[1]);
    int udp_port = std::stoi(argv[2]);
    std::string peer_id = argv[3];
    std::filesystem::path output_dir = std::filesystem::current_path() / "out";
    bool is_debug_mode = false;
    if (has_arg(argc, argv, "--debug")) {
        is_debug_mode = true;
    }

    Log::init(peer_id, output_dir / "node_log.txt", is_debug_mode);

    Log::info("main", {}, "Welcome to MeshNetworking!");

    bool use_tls = false;
    std::string cert, key, ca;
    if (argc >= 8 && std::string(argv[4]) == "--tls") {
        use_tls = true;
        cert = argv[5];
        key = argv[6];
        ca = argv[7];
    }

    boost::asio::io_context ioc;

    std::shared_ptr<boost::asio::ssl::context> ssl_ctx = nullptr;
    if (use_tls) {
        try {
            ssl_ctx = make_ssl_context(cert, key, ca);
        } catch (const std::exception &e) {
            std::cerr << e.what() << std::endl;
            return 1;
        }
    }

    MeshNode node(ioc, tcp_port, udp_port, peer_id, ssl_ctx);

    node.set_output_directory(output_dir);

    node.start();

    std::thread t([&ioc]() { ioc.run(); });

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.rfind("connect ", 0) == 0) {
            // Command: connect <host> <port>
            std::istringstream iss(line.substr(8));
            std::string host;
            int port;
            std::string new_peer_id;
            if (iss >> host >> port >> new_peer_id) {
                node.connect_to(host, port);
            } else {
                std::cout << "Usage: connect <host> <port>\n";
            }
        } else if (line.rfind("dm ", 0) == 0) {
            auto pos = line.find(' ', 3);
            if (pos != std::string::npos) {
                std::string target = line.substr(3, pos - 3);
                std::string msg = line.substr(pos + 1);
                node.send_message(target, msg);
            }
        } else if (line.rfind("broadcast ", 0) == 0) {
            // node.broadcast_text(line.substr(10));
        } else if (line.starts_with("topology")) {
            std::string arg = line.substr(std::string("topology").size());
            std::istringstream iss(arg);

            std::string dest;
            iss >> dest;

            if (dest.empty()) {
                std::cout << "Usage: topology <output_path>\n";
            }
            node.generate_topology_graph(dest);
        } else if (line.starts_with("ping")) {
            std::string arg = line.substr(std::string("ping").size());
            std::istringstream iss(arg);

            std::string peer;
            iss >> peer;
            node.ping(peer);
        } else if (line == "quit" || line == "exit") break;
        else std::cout << "Commands: broadcast <text>, dm <peer> <text>, exit\n";
    }

    node.stop();
    ioc.stop();
    t.join();
    return 0;
}
