# MeshNetworking

A decentralized, peer-to-peer mesh networking library in modern C++23. Nodes self-organize into a network and route packets through multiple hops with no central server required.

A use case is off-grid hardware deployments: embedded Linux devices communicating over a local WiFi mesh, Ethernet, or IP-based radio links. If enabled with TLS, each node carries its own certificate and a copy of the shared CA root, so authentication and encryption works without a central PKI server.


## Table of Contents

- [Architecture](#architecture)
- [Installation](#installation)
- [Usage](#usage)
  - [Starting a Node](#starting-a-node)
  - [Connecting to Peers](#connecting-to-peers)
  - [Sending Messages](#sending-messages)
  - [Request-Response Patterns](#request-response-patterns)
  - [Registering Handlers](#registering-handlers)
  - [Broadcast](#broadcast)
  - [Encryption](#encryption)
  - [Topology Visualization](#topology-visualization)
- [Configuration](#configuration)
- [Project Structure](#project-structure)
- [Building as a Library](#building-as-a-library)
- [Dependencies](#dependencies)


## Architecture

The system is organized as a layered stack:

```
┌───────────────────────────────────────────┐
│          MeshNode (Application API)       │
│   send_message / send_request / on<T>()   │
└──────────────────┬────────────────────────┘
                   │
┌──────────────────▼────────────────────────┐
│       MeshRouter (Routing Layer)          │
│   Distance-vector protocol, forwarding,   │
│       deduplication, TTL management       │
└──────────────────┬────────────────────────┘
                   │
┌──────────────────▼────────────────────────┐
│       RpcManager (Transport Layer)        │
│   TCP/UDP connections, handshake,         │
│       heartbeat, auto-reconnect           │
└──────────────────┬────────────────────────┘
                   │
┌──────────────────▼────────────────────────┐
│      Stream / Session (Network Layer)     │
│       Message framing, TLS, raw I/O       │
└───────────────────────────────────────────┘
```

**Packet flow:** `MeshNode` serializes a protobuf message into a `RoutedPacket`. `MeshRouter` looks up the next hop in its forwarding table and sends it down to `RpcManager`, which writes it to the appropriate TCP or UDP transport. At each intermediate node the router decrements TTL, checks the dedup set, and forwards again until the destination is reached.

## Installation

### Prerequisites

| Tool | Version |
|------|---------|
| C++ compiler | C++23  |
| CMake | 3.20+ |
| Boost | 1.82+ (asio, filesystem, graph) |
| Protobuf | 3.21+ |
| OpenSSL | 3.x |
| spdlog | 1.11+ |
| nlohmann_json | 3.x |
| yaml-cpp | 0.7+ |

### Build

```bash
git clone https://github.com/yourname/MeshNetworking.git
cd MeshNetworking
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

The build produces a `mesh_core` static library and a `mesh_node` example binary.



### Building as a Library

The `mesh_core` target can be embedded in other CMake projects:

```cmake
add_subdirectory(MeshNetworking)
target_link_libraries(my_app PRIVATE mesh_core)
```

All public headers are under `src/` and protobuf-generated headers are added to the include path automatically.

## Usage

### Starting a Node

```cpp
#include <mesh/node/MeshNode.h>

boost::asio::io_context ioc;
auto ssl_ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls);
std::shared_ptr<IdentityManager> identity = std::make_unique<IdentityManager>();

auto node = std::make_shared<MeshNode>(ioc, /*tcp_port=*/8080, /*udp_port=*/8081, "node-A",
                                       ssl_ctx, identity, /*encrypt_messages=*/false);


// Run the I/O loop (usually on a background thread)
std::thread io_thread([&ioc] { ioc.run(); });
node->start();
```

### Connecting to Peers
---
```cpp
// Manually connect to a known peer
node.connect("192.168.1.10", 8080);

// Register an address for automatic reconnection on startup/disconnect
node.add_auto_connection("192.168.1.10", 8080);
```

Once connected, routing tables propagate automatically — `node-A` can reach nodes that `192.168.1.10` knows about without any additional setup.

### Sending Messages

Any protobuf message type can be sent directly. The library uses the message's fully-qualified type name to route it to the correct handler on the other side.

```cpp
// Fire-and-forget over TCP (default)
ProtoMessage msg;
msg.set_content("hello");
node.send_message<ProtoMessage>("node-B", msg);

// Send over UDP instead
node.send_message<ProtoMessage>("node-B", msg, TransportProtocol::UDP);

// Send plain text - mainly for debug.
node.send_text("node-B", "ping!");
```

### Request-Response Patterns

#### Async callback

```cpp
MyRequest req;
req.set_query("what is 2+2?");

node.send_request_async<MyRequest, MyResponse>(
    "node-B", req,
    [](const std::string& from, std::expected<MyResponse, RequestError> result) {
        if (result) {
            std::cout << "Got answer: " << result->answer() << "\n";
        } else {
            std::cerr << "Request timed out\n";
        }
    },
    /*timeout=*/std::chrono::seconds(5)
);
```

#### Future-based (blocking)

```cpp
auto future = node.send_request<MyRequest, MyResponse>("node-B", req, 5s);
auto result = future.get();  // blocks until response or timeout

if (result) {
    auto [peer_id, response] = *result;
    std::cout << peer_id << " replied: " << response.answer() << "\n";
}
```

### Registering Handlers

```cpp
// Handler that sends a response
node.on<MyRequest, MyResponse>(
    [](const std::string& from, const MyRequest& req, auto reply) {
        MyResponse resp;
        resp.set_answer("4");
        reply(resp.SerializeAsString());
    }
);

// One-way handler (no response expected)
node.on<MyMessage>(
    [](const std::string& from, const MyMessage& msg) {
        std::cout << from << " says: " << msg.content() << "\n";
    }
);
```

### Broadcast

Send to every node in the network. Packets flood outward with TTL-limited hops; the dedup layer ensures each node processes the packet exactly once.

```cpp
// Broadcast message (fire-and-forget)
node.broadcast_message<Announcement>(announcement);

// Broadcast request — collect all responses within the timeout window
auto future = node.broadcast_request<StatusRequest, StatusResponse>(req, 5s);
auto responses = future.get();  // std::vector<std::pair<peer_id, StatusResponse>>

for (auto& [peer, status] : responses) {
    std::cout << peer << ": " << status.uptime_seconds() << "s uptime\n";
}
```

### Encryption

End-to-end encryption is opt-in and transparent to message handlers. When enabled, the library:

1. Generates a random AES-256 session key per packet
2. Encrypts the payload with AES-256-CBC
3. Wraps the session key with the recipient's RSA public key
4. Signs the ciphertext with the sender's private key

Public keys are exchanged automatically on first contact using an [IdentityRequest/IdentityResponse](/proto/crypto.proto) handshake. Outgoing packets are buffered until the key exchange completes.

Intermediate routing nodes never see plaintext; they only forward encrypted `RoutedPacket` bytes.

#### Generating Certificates

Each node needs its own RSA key and certificate signed by a shared CA. The `scripts/gen_certs.py` script handles this automatically.

Install the script's dependency first:

```bash
pip install -r scripts/requirements.txt
```

Then generate a certificate for each node by passing its name (used as the CN and peer ID):

```bash
python scripts/gen_certs.py node-A
python scripts/gen_certs.py node-B
python scripts/gen_certs.py node-C
```

On the first run the script creates a self-signed Root CA (`certs/ca_cert.pem` + `certs/ca_key.pem`) valid for 10 years. Each subsequent call signs a new node certificate against that CA. The output for each node is placed in `certs/<node-name>/`:

```
certs/
├── ca_cert.pem          # shared Root CA certificate
├── ca_key.pem           # CA private key (keep secret)
├── node-A/
│   ├── node_cert.pem    # node-A's certificate (signed by CA)
│   ├── node_key.pem     # node-A's RSA private key
│   └── ca_root.pem      # copy of the CA cert for peer verification
├── node-B/
│   └── ...
```

Pass these paths when constructing the node:

```cpp
auto ssl_ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls);
auto identity = std::make_shared<IdentityManager>(
    "certs/node-A/node_cert.pem",
    "certs/node-A/node_key.pem",
    "certs/node-A/ca_root.pem"
);

auto node = std::make_shared<MeshNode>(ioc, 8080, 8081, "node-A",
                                       ssl_ctx, identity, /*encrypt_messages=*/true);
```


> **Note:** Because this project is primarily served for off-grid nodes which don't have access to a remote Central Authority, all nodes in the network must share the same Root CA for mutual authentication to succeed.
### Topology Visualization

```cpp
// Query all reachable nodes and export a GraphViz file
node.generate_topology_graph("topology.dot");
```

Then render with GraphViz:

```bash
dot -Tpng topology.dot -o topology.png
```

## Configuration

Nodes can be configured with a YAML file:

```yaml
peer-id: "node-A"
tcp-port: 8080
udp-port: 8081
debug: true
output-dir: "./out"

tls:
  cert-path: "certs/node-a.crt"
  key-path:  "certs/node-a.key"
  ca-path:   "certs/ca.crt"
  encrypt-messages: true

auto_connect:
  - ip: 127.0.0.1
    port: 9090
  - ip: 192.168.1.20
    port: 8080
```

The example `main.cpp` binary reads this config at startup:

```bash
./mesh_node config/node-a.yaml
```

Run a dev build with:
```bash
./dev_run config/node-a.yaml
````
