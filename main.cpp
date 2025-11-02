#include <iostream>
#include <thread>
#include <boost/asio.hpp>

#include "mesh_node.h"

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "Usage: meshnode <tcp_port> <udp_discovery_port> <peer_id> [--tls cert key ca]\n";
        return 1;
    }
    int tcp_port = std::stoi(argv[1]);
    int udp_port = std::stoi(argv[2]);
    std::string peer_id = argv[3];

    bool use_tls = false;
    std::string cert, key, ca;
    if (argc >= 8 && std::string(argv[4]) == "--tls") {
        use_tls = true;
        cert = argv[5];
        key = argv[6];
        ca = argv[7];
    }

    boost::asio::io_context ioc;
    MeshNode node(ioc, tcp_port, udp_port, peer_id);

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
        } else if (line == "quit" || line == "exit") break;
        else std::cout << "Commands: broadcast <text>, dm <peer> <text>, exit\n";
    }

    node.stop();
    ioc.stop();
    t.join();
    return 0;
}
