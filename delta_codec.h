// delta_codec.h — Step 4: telemetry-specific compression
//
// XOR-delta encoding, the core trick behind Facebook's Gorilla time-series
// compression. Consecutive sensor readings usually share the same exponent
// and top mantissa bits (e.g. 10.01, 10.02, 10.03 are all close in
// magnitude), so XOR-ing consecutive IEEE-754 bit patterns produces mostly
// leading zero bytes. We strip those and store only what's left.
//
// This is LOSSLESS: decode() reproduces the exact original bit pattern,
// not an approximation. That's the advantage over naive quantize-and-round
// delta encoding.

#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

inline uint64_t d2bits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }
inline double bits2d(uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; }

// Layout: [first value: 8 raw bytes]
//         then per subsequent value: [1 header byte = leading-zero-byte count 0..8]
//                                     [(8 - header) significant bytes, MSB-first]
inline std::vector<uint8_t> delta_encode(const double *values, size_t n) {
    std::vector<uint8_t> out;
    if (n == 0) return out;

    out.resize(8);
    std::memcpy(out.data(), &values[0], 8);
    uint64_t prev = d2bits(values[0]);

    for (size_t i = 1; i < n; ++i) {
        uint64_t cur = d2bits(values[i]);
        uint64_t x = cur ^ prev;

        uint8_t lz_bytes = 0;
        if (x == 0) {
            lz_bytes = 8;
        } else {
            for (int b = 7; b >= 0; --b) {
                if (((x >> (b * 8)) & 0xFF) != 0) break;
                lz_bytes++;
            }
        }
        out.push_back(lz_bytes);
        int sig_bytes = 8 - lz_bytes;
        for (int b = sig_bytes - 1; b >= 0; --b)
            out.push_back((uint8_t)((x >> (b * 8)) & 0xFF));

        prev = cur;
    }
    return out;
}

inline std::vector<double> delta_decode(const std::vector<uint8_t> &buf, size_t n) {
    std::vector<double> out;
    if (n == 0 || buf.size() < 8) return out;
    out.reserve(n);

    double first;
    std::memcpy(&first, buf.data(), 8);
    out.push_back(first);
    uint64_t prev = d2bits(first);

    size_t pos = 8;
    for (size_t i = 1; i < n; ++i) {
        uint8_t lz_bytes = buf[pos++];
        int sig_bytes = 8 - lz_bytes;
        uint64_t x = 0;
        for (int b = 0; b < sig_bytes; ++b)
            x = (x << 8) | buf[pos++];
        uint64_t cur = prev ^ x;
        out.push_back(bits2d(cur));
        prev = cur;
    }
    return out;
}
