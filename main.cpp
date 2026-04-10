#include <filesystem>
#include <iostream>
#include <thread>
#include <boost/asio.hpp>

#include "mesh/crypto/CertHelpers.h"
#include "mesh/logging/Logger.h"
#include "yaml-cpp/yaml.h"
#include "mesh/node/MeshNode.h"

std::vector<std::string> tokenize(const std::string &line) {
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string word;
    while (iss >> word) {
        tokens.push_back(word);
    }
    return tokens;
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

    std::filesystem::path output_dir =
            config["output-dir"]
                ? std::filesystem::path(config["output-dir"].as<std::string>())
                : std::filesystem::current_path() / "out";

    try {
        std::filesystem::create_directories(output_dir);
    } catch (const std::filesystem::filesystem_error &e) {
        std::cerr << "Error creating directory: " << e.what() << std::endl;
    }

    bool is_debug_mode = config["debug"] ? config["debug"].as<bool>() : false;

    Log::init(peer_id, output_dir / "node_log.txt", is_debug_mode);
    Log::info("main", {}, "Welcome to MeshNetworking!");

    bool use_tls = false;
    bool encrypt_messages = false;
    std::string cert, key, ca;

    if (config["tls"] &&
        config["tls"]["cert-path"] &&
        config["tls"]["key-path"] &&
        config["tls"]["ca-path"]) {
        use_tls = true;
        cert = config["tls"]["cert-path"].as<std::string>();
        key = config["tls"]["key-path"].as<std::string>();
        ca = config["tls"]["ca-path"].as<std::string>();

        if (config["tls"]["encrypt-messages"]) {
            encrypt_messages = true;
        }
    }

    std::shared_ptr<boost::asio::ssl::context> ssl_ctx = nullptr;
    auto identity_manager = std::make_shared<IdentityManager>();

    if (use_tls) {
        try {
            ssl_ctx = CertHelpers::make_ssl_context(cert, key, ca);
        } catch (const std::exception &e) {
            std::cerr << e.what() << std::endl;
            return 1;
        }
    }

    if (encrypt_messages) {
        identity_manager->load_identities(cert, key, ca);
    }

    boost::asio::io_context ioc;
    MeshNode node(ioc, tcp_port, udp_port, peer_id, ssl_ctx, identity_manager, encrypt_messages);

    node.set_output_directory(output_dir);
    node.start();

    if (config["auto_connect"]) {
        for (const auto &elem: config["auto_connect"]) {
            if (!elem["ip"] || !elem["port"]) {
                Log::warn("yaml_parse", {}, "Malformed auto_connect entry");
                continue;
            }

            std::string ip = elem["ip"].as<std::string>();
            int port = elem["port"].as<int>();

            node.add_auto_connection(ip, port);
        }
    }

    std::thread io_thread([&ioc]() { ioc.run(); });

    using CommandHandler = std::function<void(const std::vector<std::string> &)>;
    std::unordered_map<std::string, CommandHandler> commands;

    commands["connect"] = [&](const std::vector<std::string> &args) {
        if (args.size() < 2) {
            std::cout << "Usage: connect <host> <port>\n";
            return;
        }
        node.connect(args[0], std::stoi(args[1]));
    };

    commands["dm"] = [&](const std::vector<std::string> &args) {
        if (args.size() < 2) {
            std::cout << "Usage: dm <peer> <message>\n";
            return;
        }

        std::string target = args[0];
        std::string msg;

        for (size_t i = 1; i < args.size(); ++i) {
            if (i > 1) msg += " ";
            msg += args[i];
        }

        node.send_text(target, msg);
    };

    commands["topology"] = [&](const std::vector<std::string> &args) {
        if (args.empty()) {
            std::cout << "Usage: topology <output_path>\n";
            return;
        }
        node.generate_topology_graph(args[0]);
    };

    commands["ping"] = [&](const std::vector<std::string> &args) {
        if (args.empty()) {
            std::cout << "Usage: ping <peer>\n";
            return;
        }
        node.ping(args[0]);
    };

    commands["get_nodes"] = [&](const std::vector<std::string> &) {
        auto nodes = node.get_nodes_in_network();
        std::cout << nodes.size() << " node(s)\n";
        for (const auto &n: nodes) {
            std::cout << n << "\n";
        }
    };

    commands["block"] = [&](const std::vector<std::string> &) {
        node.set_block_all_messages(true);
    };

    commands["unblock"] = [&](const std::vector<std::string> &) {
        node.set_block_all_messages(false);
    };

    commands["help"] = [&](const std::vector<std::string> &) {
        std::cout << "Available commands:\n";
        for (const auto &[name, _]: commands) {
            std::cout << " - " << name << "\n";
        }
    };

    std::string line;
    while (std::getline(std::cin, line)) {
        auto tokens = tokenize(line);
        if (tokens.empty()) continue;

        std::string cmd = tokens[0];
        tokens.erase(tokens.begin());

        if (cmd == "exit" || cmd == "quit") {
            break;
        }

        auto it = commands.find(cmd);
        if (it != commands.end()) {
            it->second(tokens);
        } else {
            std::cout << "Unknown command. Type 'help'\n";
        }
    }

    node.stop();
    ioc.stop();
    io_thread.join();

    return 0;
}
