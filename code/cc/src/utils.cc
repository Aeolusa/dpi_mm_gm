// ============================================================
// file: src/utils.cc
// ============================================================
#include "utils.h"
#include <stdexcept>

static uint8_t hex_char_to_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw std::invalid_argument("Invalid hex character");
}

Data_t hex_to_bytes(const std::string& hex_str) {
    Data_t bytes;
    // Prefix "0x" logic ignores
    size_t start = 0;
    if (hex_str.size() >= 2 && hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X')) {
        start = 2;
    }

    size_t len = hex_str.length() - start;
    if (len == 0) return bytes;

    // Handle odd length by padding a leading 0 implicitely
    // For example, "F" -> 0x0F
    bool has_odd = (len % 2 != 0);
    size_t num_bytes = (len + 1) / 2;
    bytes.reserve(num_bytes);

    size_t i = start;
    if (has_odd) {
        bytes.push_back(hex_char_to_val(hex_str[i]));
        i++;
    }

    while (i < hex_str.length()) {
        uint8_t high = hex_char_to_val(hex_str[i]);
        uint8_t low = hex_char_to_val(hex_str[i+1]);
        bytes.push_back((high << 4) | low);
        i += 2;
    }
    
    // Reverse because AXI/CHI byte 0 is usually the right-most, wait
    // Actually, usually string "AABB" means AA is byte 1, BB is byte 0 if little endian
    // It depends on the DPI conventions. Let's just store as-is first (big-endian string -> array [0]=MSB), 
    // or reverse it based on Little Endian. SystemVerilog usually puts [0] at right.
    std::reverse(bytes.begin(), bytes.end());

    return bytes;
}

ByteEn_t hex_to_byte_en(const std::string& hex_str, size_t target_len) {
    Data_t bytes = hex_to_bytes(hex_str);
    ByteEn_t be;
    be.reserve(target_len);

    for (size_t i = 0; i < target_len; i++) {
        size_t byte_idx = i / 8;
        size_t bit_idx = i % 8;
        if (byte_idx < bytes.size()) {
            bool enabled = (bytes[byte_idx] & (1 << bit_idx)) != 0;
            be.push_back(enabled);
        } else {
            be.push_back(false);
        }
    }
    return be;
}
