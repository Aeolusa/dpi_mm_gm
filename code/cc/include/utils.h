// ============================================================
// file: utils.h
// Utilities for data conversion
// ============================================================
#pragma once

#include "types.h"
#include <string>

// Converts a hex string (e.g., "deadbeef" or "DEADBEEF") to a byte array.
Data_t hex_to_bytes(const std::string& hex_str);

// Converts a hex string of byte enables to a bool vector.
// Expected to be up to target_len.
ByteEn_t hex_to_byte_en(const std::string& hex_str, size_t target_len);
