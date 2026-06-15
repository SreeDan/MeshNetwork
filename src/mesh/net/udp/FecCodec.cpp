#include "mesh/net/udp/FecCodec.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>

FecCodec::FecCodec(uint8_t k, uint8_t m)
    : k_(k), m_(m) {

    if (k_ < 1) {
        throw std::invalid_argument("FecCodec requires at least one data packet per group");
    }
    if (m_ < 1) {
        throw std::invalid_argument("FecCodec requires at least one parity packet");
    }
    if (static_cast<int>(k_) + m_ > 254) {
        throw std::invalid_argument("FecCodec requires data packets + roots <= 254");
    }

    build_gf_tables();
    build_enc_matrix();

    send_buf_.reserve(k_);
    send_lens_.reserve(k_);
}

// GF(2^8) arithmetic - primitive polynomial x^8+x^4+x^3+x^2+1 = 0x11D
void FecCodec::build_gf_tables() {
    uint32_t x = 1;
    for (int i = 0; i < 255; ++i) {
        gf_exp_[i] = static_cast<uint8_t>(x);
        gf_log_[x] = static_cast<uint8_t>(i);
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    // Double the exp table to avoid modulo in gf_mul.
    for (int i = 255; i < 512; ++i) {
        gf_exp_[i] = gf_exp_[i - 255];
    }

    gf_log_[0] = 0; // undefined — only accessed if code has a bug
}

uint8_t FecCodec::gf_mul(uint8_t a, uint8_t b) const {
    if (a == 0 || b == 0) return 0;
    return gf_exp_[static_cast<int>(gf_log_[a]) + gf_log_[b]];
}

uint8_t FecCodec::gf_inv(uint8_t a) const {
    assert(a != 0);
    return gf_exp_[255 - gf_log_[a]];
}

// Cauchy encoding matrix
// enc_[j][i] = 1 / (x_i XOR y_j)  where x_i = i, y_j = k_+j
void FecCodec::build_enc_matrix() {
    enc_.assign(m_, std::vector<uint8_t>(k_));
    for (int j = 0; j < m_; ++j) {
        for (int i = 0; i < k_; ++i) {
            const uint8_t denom = static_cast<uint8_t>(i) ^ static_cast<uint8_t>(k_ + j);
            enc_[j][i] = gf_inv(denom);
        }
    }
}

std::string FecCodec::make_header(uint32_t group_id, uint8_t seq,
                                   uint8_t num_data, uint8_t num_roots,
                                   uint16_t data_len) {
    std::string hdr(kHeaderSize, '\0');
    hdr[0] = 'M';
    hdr[1] = 'F';
    hdr[2] = 'E';
    hdr[3] = 'C';
    hdr[4] = 1;
    hdr[5] = static_cast<char>((group_id >> 24) & 0xFF);
    hdr[6] = static_cast<char>((group_id >> 16) & 0xFF);
    hdr[7] = static_cast<char>((group_id >> 8) & 0xFF);
    hdr[8] = static_cast<char>(group_id & 0xFF);
    hdr[9] = static_cast<char>(seq);
    hdr[10] = static_cast<char>(num_data);
    hdr[11] = static_cast<char>(num_roots);
    hdr[12] = static_cast<char>((data_len >> 8) & 0xFF);
    hdr[13] = static_cast<char>(data_len & 0xFF);
    return hdr;
}

bool FecCodec::is_fec_frame(const std::string& datagram) {
    return datagram.size() >= kHeaderSize &&
           datagram[0] == 'M' &&
           datagram[1] == 'F' &&
           datagram[2] == 'E' &&
           datagram[3] == 'C' &&
           static_cast<uint8_t>(datagram[4]) == 1;
}

std::optional<std::pair<uint8_t, uint8_t>> FecCodec::frame_config(const std::string& datagram) {
    if (!is_fec_frame(datagram)) {
        return std::nullopt;
    }

    return std::make_pair(static_cast<uint8_t>(datagram[10]), static_cast<uint8_t>(datagram[11]));
}

bool FecCodec::parse_header(const std::string& dg,
                             uint32_t& group_id, uint8_t& seq,
                             uint8_t& num_data, uint8_t& num_roots,
                             uint16_t& data_len) {
    if (dg.size() < kHeaderSize)
        return false;

    if (!is_fec_frame(dg))
        return false;

    const auto* d = reinterpret_cast<const uint8_t*>(dg.data());
    group_id = (uint32_t(d[5]) << 24) | (uint32_t(d[6]) << 16)
             | (uint32_t(d[7]) <<  8) | uint32_t(d[8]);

    seq = d[9];
    num_data = d[10];
    num_roots = d[11];
    data_len = (uint16_t(d[12]) << 8) | d[13];
    return true;
}

std::vector<std::vector<uint8_t>> FecCodec::compute_parity(
        const std::vector<std::string>& data) const {

    size_t max_len = 0;
    for (const auto& d : data) {
        max_len = std::max(max_len, d.size());
    }

    // parity_vecs[j] = GF linear combination of all data packets for parity row j
    std::vector<std::vector<uint8_t>> parity_vecs(m_, std::vector<uint8_t>(max_len, 0));

    for (int j = 0; j < m_; ++j) {
        for (int i = 0; i < k_; ++i) {
            const uint8_t coeff = enc_[j][i];
            for (size_t b = 0; b < max_len; ++b) {
                const uint8_t db = b < data[i].size()
                                   ? static_cast<uint8_t>(data[i][b]) : 0;
                parity_vecs[j][b] ^= gf_mul(coeff, db);
            }
        }
    }
    return parity_vecs;
}

FecCodec::EncodeResult FecCodec::encode(const std::string& payload) {
    const size_t max_payload_len = std::numeric_limits<uint16_t>::max() - (2u * k_);
    if (payload.size() > max_payload_len) {
        throw std::invalid_argument("FEC payload is too large for the UDP FEC frame format");
    }
    const uint16_t plen = static_cast<uint16_t>(payload.size());

    EncodeResult result;
    result.data_frame = make_header(send_group_id_, send_seq_, k_, m_, plen) + payload;

    // Buffer a copy for parity computation.
    send_buf_.push_back(payload);
    send_lens_.push_back(plen);
    ++send_seq_;

    if (send_seq_ == k_) {
        // Group complete: compute and emit m parity frames.
        const auto parity_vecs = compute_parity(send_buf_);

        for (int j = 0; j < m_; ++j) {
            std::string parity_payload;
            parity_payload.reserve(2 * k_ + parity_vecs[j].size());

            for (uint16_t len : send_lens_) {
                parity_payload += static_cast<char>((len >> 8) & 0xFF);
                parity_payload += static_cast<char>( len       & 0xFF);
            }

            parity_payload.append(reinterpret_cast<const char*>(parity_vecs[j].data()), parity_vecs[j].size());

            if (parity_payload.size() > std::numeric_limits<uint16_t>::max()) {
                throw std::invalid_argument("FEC parity payload is too large for the UDP FEC frame format");
            }

            const uint16_t plen_p = static_cast<uint16_t>(parity_payload.size());
            const uint8_t  parity_seq = static_cast<uint8_t>(k_ + j);
            result.parity_frames.push_back(make_header(send_group_id_, parity_seq, k_, m_, plen_p) + parity_payload);
        }

        // Reset for the next group.
        ++send_group_id_;
        send_seq_ = 0;
        send_buf_.clear();
        send_lens_.clear();
    }

    return result;
}

// Gaussian elimination decoder over GF(2^8)
//
// Solves  M * X = B  for X (the k data byte-vectors).
// M is a k×k matrix built from the rows of the full encoding matrix that
// correspond to the k received packets. B holds the received byte data.
std::vector<std::vector<uint8_t>> FecCodec::gauss_decode(
        const std::vector<int>& received_indices,
        const std::vector<std::vector<uint8_t>>& received_data,
        size_t symbol_len) const {

    const int k = k_;

    // Build k×k matrix M and k×symbol_len matrix B (working copies).
    std::vector<std::vector<uint8_t>> M(k, std::vector<uint8_t>(k, 0));
    std::vector<std::vector<uint8_t>> B = received_data;

    for (int row = 0; row < k; ++row) {
        const int idx = received_indices[row];
        if (idx < k) {
            // Data packet: row in full encoding matrix is a unit vector.
            M[row][idx] = 1;
        } else {
            // Parity packet: row is enc_[j] where j = idx - k.
            const int j = idx - k;
            M[row] = enc_[j]; // length k_
        }
    }

    // Gauss-Jordan elimination (in-place on M and B simultaneously).
    for (int col = 0; col < k; ++col) {
        // Find a non-zero pivot in this column.
        int pivot = -1;
        for (int row = col; row < k; ++row) {
            if (M[row][col] != 0) { pivot = row; break; }
        }
        if (pivot == -1) return {}; // singular, received packets are linearly dependent

        if (pivot != col) {
            std::swap(M[col],  M[pivot]);
            std::swap(B[col],  B[pivot]);
        }

        // Scale pivot row so M[col][col] == 1.
        const uint8_t scale = gf_inv(M[col][col]);
        for (int c = col; c < k; ++c)       M[col][c]  = gf_mul(M[col][c],  scale);
        for (size_t b = 0; b < symbol_len; ++b) B[col][b] = gf_mul(B[col][b], scale);

        // Eliminate all other rows in this column.
        for (int row = 0; row < k; ++row) {
            if (row == col || M[row][col] == 0) continue;
            const uint8_t factor = M[row][col];
            for (int c = col; c < k; ++c)
                M[row][c] ^= gf_mul(factor, M[col][c]);
            for (size_t b = 0; b < symbol_len; ++b)
                B[row][b] ^= gf_mul(factor, B[col][b]);
        }
    }

    // B now holds the k data byte-vectors in their original order.
    return B;
}

std::vector<std::string> FecCodec::decode(const std::string& datagram) {
    uint32_t group_id;
    uint8_t  seq, num_data, num_roots;
    uint16_t data_len;

    if (!parse_header(datagram, group_id, seq, num_data, num_roots, data_len))
        return {};

    if (num_data != k_ || num_roots != m_)
        return {};

    if (seq >= static_cast<uint8_t>(num_data + num_roots))
        return {};

    if (datagram.size() < kHeaderSize + data_len)
        return {};

    const std::string payload = datagram.substr(kHeaderSize, data_len);

    auto& group = recv_groups_[group_id];
    if (group.delivered) return {};

    if (group.k == 0) {
        group.k = num_data;
        group.m = num_roots;
        group.slots.resize(num_data + num_roots);
        group.data_lens.resize(num_data);
        remember_receive_group(group_id);
    }

    const bool is_parity = (seq >= num_data);

    if (seq >= static_cast<uint8_t>(group.slots.size())) return {};
    if (group.slots[seq].has_value()) return {}; // duplicate

    if (is_parity) {
        // Parse length manifest from the front of the parity payload.
        const size_t manifest_bytes = 2u * num_data;

        if (payload.size() < manifest_bytes)
            return {};

        for (int i = 0; i < num_data; ++i) {
            group.data_lens[i] =
                (uint16_t(static_cast<uint8_t>(payload[2 * i])) << 8)
                |  uint16_t(static_cast<uint8_t>(payload[2 * i + 1]));
        }
        // Store the parity vector only (strip the manifest prefix).
        group.slots[seq] = payload.substr(manifest_bytes);
    } else {
        group.data_lens[seq] = data_len;
        group.slots[seq] = payload;
    }

    ++group.received;
    return try_deliver(group_id);
}

std::vector<std::string> FecCodec::try_deliver(uint32_t group_id) {
    auto it = recv_groups_.find(group_id);

    if (it == recv_groups_.end())
        return {};

    auto& group = it->second;
    if (group.delivered || group.received < group.k)
        return {};

    // Collect the first k received slots and their indices.
    std::vector<int> indices;
    std::vector<std::vector<uint8_t>> raw_data;
    indices.reserve(group.k);
    raw_data.reserve(group.k);

    // Determine max parity length for the symbol_len used in decoding.
    size_t symbol_len = 0;
    for (int i = 0; i < group.k + group.m; ++i) {
        if (group.slots[i].has_value()) {
            symbol_len = std::max(symbol_len, group.slots[i]->size());
        }
    }

    for (int i = 0; i < group.k + group.m && static_cast<int>(indices.size()) < group.k; ++i) {
        if (!group.slots[i].has_value()) continue;
        indices.push_back(i);
        // Zero-pad each received packet to symbol_len.
        const auto& s = *group.slots[i];
        std::vector<uint8_t> v(symbol_len, 0);
        std::copy(s.begin(), s.end(), v.begin());
        raw_data.push_back(std::move(v));
    }

    const auto recovered_vecs = gauss_decode(indices, raw_data, symbol_len);
    if (recovered_vecs.empty()) // shouldn't happen with valid Cauchy
        return {};

    group.delivered = true;

    // Convert byte-vectors back to strings, truncating to original lengths.
    std::vector<std::string> result;
    result.reserve(group.k);
    for (int i = 0; i < group.k; ++i) {
        const size_t orig_len = group.data_lens[i] == 0 && recovered_vecs[i].empty() ? 0 : static_cast<size_t>(group.data_lens[i]);
        const size_t actual = std::min(orig_len, recovered_vecs[i].size());
        result.emplace_back(reinterpret_cast<const char*>(recovered_vecs[i].data()), actual);
    }

    recv_groups_.erase(it);
    return result;
}

void FecCodec::remember_receive_group(uint32_t group_id) {
    recv_group_order_.push_back(group_id);

    while (recv_group_order_.size() > kMaxReceiveGroups) {
        const uint32_t stale_group_id = recv_group_order_.front();
        recv_group_order_.pop_front();
        if (stale_group_id != group_id) {
            recv_groups_.erase(stale_group_id);
        }
    }
}
