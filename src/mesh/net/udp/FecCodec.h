#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Reed-Solomon FEC over GF(2^8) with k data packets + m parity packets per group.
// Any k of the k + m transmitted frames are enought to recover all k original packets,
// so up to m arbitrary packet losses per group are recoverable.
//
// Uses a Cauchy encoding matrix, which guarantees every k×k submatrix is
// invertible over GF(2^8). Decoding is Gaussian elimination over GF(2^8).
//
// Wire format: every UDP datagram has a 14-byte FEC header:
//   [magic] - 4 bytes: "MFEC"
//   [version] - 1 byte: 1
//   [group_id] - 4 bytes: big-endian
//   [seq] - 1 byte: 0...k-1 = data index, k..k+m-1 = parity index
//   [num_data] - 1 byte: k
//   [num_roots] - 1 byte: m
//   [data_len] - 2 bytes: big-endian byte length of the following payloa:
//
// Parity packet payload:
//   [len_0 : 2 bytes] ... [len_{k-1} : 2 bytes]: original length of each data packet
//   [parity_bytes : max(len_i) bytes]: GF(2^8) linear combination of data packets
//
// Data packet payload: the raw serialized RoutedPacket bytes (unchanged).
//
// Constraint: k + m <= 254  (GF(2^8) has 255 nonzero elements)
class FecCodec {
public:
    static constexpr size_t kHeaderSize = 14;
    static constexpr size_t kMaxReceiveGroups = 256;

    // k = data packets per group, m = parity packets (roots).
    // Default k=4 m=2: tolerates any 2 lost packets per group of 6.
    FecCodec(uint8_t k = 4, uint8_t m = 2);

    struct EncodeResult {
        std::string data_frame;
        // Populated (with m frames) only when the kth data packet completes the group.
        std::vector<std::string> parity_frames;
    };

    // Wrap one outgoing payload. Returns the framed data datagram immediately
    // parity_frames is filled when the kth packet completes the group.
    EncodeResult encode(const std::string& payload);

    // Consume one raw incoming datagram. Returns recovered data payloads (in
    // order, 0..k-1) once k frames from the same group have been received.
    std::vector<std::string> decode(const std::string& datagram);

    static bool is_fec_frame(const std::string& datagram);
    static std::optional<std::pair<uint8_t, uint8_t>> frame_config(const std::string& datagram);

private:
    uint8_t k_;
    uint8_t m_;

    // GF(2^8) arithmetic tables (primitive polynomial 0x11D).
    std::array<uint8_t, 512> gf_exp_; // doubled to avoid mod in multiply
    std::array<uint8_t, 256> gf_log_;

    // Cauchy encoding matrix: enc_[j][i] = coefficient for data[i] in parity[j].
    // Dimensions: m_ rows × k_ cols.
    std::vector<std::vector<uint8_t>> enc_;

    // send side
    uint32_t send_group_id_{0};
    uint8_t  send_seq_{0};
    std::vector<std::string> send_buf_; // buffered data payloads for parity computation
    std::vector<uint16_t>    send_lens_;

    // receive side
    struct RecvGroup {
        uint8_t k{0}, m{0};
        // Slot i holds data packet i (i < k) or parity packet i-k (i >= k).
        std::vector<std::optional<std::string>> slots;
        std::vector<uint16_t> data_lens; // from any received parity packet
        int received{0};
        bool delivered{false};
    };
    std::unordered_map<uint32_t, RecvGroup> recv_groups_;
    std::deque<uint32_t> recv_group_order_;

    // GF(2^8) field operations
    void    build_gf_tables();
    uint8_t gf_mul(uint8_t a, uint8_t b) const;
    uint8_t gf_inv(uint8_t a) const;

    void build_enc_matrix();

    // Compute m parity byte-vectors from k data payloads.
    std::vector<std::vector<uint8_t>> compute_parity(
        const std::vector<std::string>& data) const;

    // Gaussian elimination decoder. `received_indices` lists which slots
    // (0 ... k+m-1) are present
    // `received_data` holds their byte vectors.
    // Returns the k recovered data byte-vectors, or empty on failure.
    std::vector<std::vector<uint8_t>> gauss_decode(
        const std::vector<int>& received_indices,
        const std::vector<std::vector<uint8_t>>& received_data,
        size_t symbol_len) const;

    std::vector<std::string> try_deliver(uint32_t group_id);
    void remember_receive_group(uint32_t group_id);

    static std::string make_header(uint32_t group_id, uint8_t seq,
                                   uint8_t num_data, uint8_t num_roots,
                                   uint16_t data_len);
    static bool parse_header(const std::string& dg,
                              uint32_t& group_id, uint8_t& seq,
                              uint8_t& num_data, uint8_t& num_roots,
                              uint16_t& data_len);
};
