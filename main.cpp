#include <filesystem>
#include <iostream>
#include <thread>
#include <boost/asio.hpp>

#include "Logger.h"
#include "yaml-cpp/yaml.h"
#include "MeshNode.h"

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

        Log::debug("tls", {}, "TLS Context initialized successfully");

        return ssl_ctx;
    } catch (const std::exception &e) {
        throw std::runtime_error("TLS Setup Failed: " + std::string(e.what()));
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: meshnode config_path.yml\n";
        return 1;
    }

    YAML::Node config = YAML::LoadFile(argv[1]);
    int tcp_port = config["tcp-port"].as<int>();
    int udp_port = config["udp-port"].as<int>();
    std::string peer_id = config["peer-id"].as<std::string>();

    std::filesystem::path output_dir;
    if (config["output-dir"]) {
        output_dir = config["output-dir"].as<std::string>();
    } else {
        output_dir = std::filesystem::current_path() / "out";
    }

    try {
        if (std::filesystem::create_directories(output_dir)) {
            std::cout << "Successfully created directory path: " << output_dir << std::endl;
        } else {
            std::cout << "Directory path already exists or a non-error condition occurred." << std::endl;
        }
    } catch (const std::filesystem::filesystem_error &e) {
        std::cerr << "Error creating directory path: " << e.what() << std::endl;
    }


    bool is_debug_mode = false;
    if (config["debug"]) {
        is_debug_mode = config["debug"].as<bool>();
    }

    Log::init(peer_id, output_dir / "node_log.txt", is_debug_mode);

    Log::info("main", {}, "Welcome to MeshNetworking!");

    bool use_tls = false;
    std::string cert, key, ca;
    if (config["tls"] && config["tls"]["cert-path"] && config["tls"]["key-path"] && config["tls"]["ca-path"]) {
        use_tls = true;
        cert = config["tls"]["cert-path"].as<std::string>();
        key = config["tls"]["key-path"].as<std::string>();
        ca = config["tls"]["ca-path"].as<std::string>();
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

    if (config["auto_connect"]) {
        const YAML::Node &auto_connect = config["auto_connect"];
        for (const auto &elem: auto_connect) {
            if (!elem["ip"] || !elem["port"]) {
                Log::warn("yaml_parse", {}, "malformed auto_conect config, should have an ip and port present");
                continue;
            }

            std::string ip = elem["ip"].as<std::string>();
            int port = elem["port"].as<int>();

            Log::debug("yaml_parser", {{"ip", ip}, {"port", port}}, "detected auto connect configuration");
            node.add_auto_connection(ip, port);
        }
    }

    std::thread t([&ioc]() { ioc.run(); });
    // TODO: Add automatic connections to config yaml

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.rfind("connect ", 0) == 0) {
            // Command: connect <host> <port>
            std::istringstream iss(line.substr(8));
            std::string host;
            int port;
            std::string new_peer_id;
            if (iss >> host >> port >> new_peer_id) {
                node.connect(host, port);
            } else {
                std::cout << "Usage: connect <host> <port>\n";
            }
        } else if (line.rfind("dm ", 0) == 0) {
            auto pos = line.find(' ', 3);
            if (pos != std::string::npos) {
                std::string target = line.substr(3, pos - 3);
                std::string msg = line.substr(pos + 1);
                node.send_text(target, msg);
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
        } else if (line == "get_nodes") {
            std::vector<std::string> nodes = node.get_nodes_in_network();
            std::cout << nodes.size() << " node(s)" << std::endl;
            for (size_t i = 0; i < nodes.size(); ++i) {
                std::cout << nodes[i] << std::endl;
            }
        } else if (line == "block") {
            node.set_block_all_messages(true);
        } else if (line == "unblock") {
            node.set_block_all_messages(false);
        } else if (line == "quit" || line == "exit") break;
        else std::cout << "Commands: broadcast <text>, dm <peer> <text>, exit\n";
    }

    node.stop();
    ioc.stop();
    t.join();
    return 0;
}
