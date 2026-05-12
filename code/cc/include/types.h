// ============================================================
// file: types.h
// basic type definition
// ============================================================
#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <deque>
#include <optional>
#include <memory>
#include <functional>
#include <cassert>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <set>
#include <mutex>

// ---------- 基本类型别名 ----------
using Addr_t    = uint64_t;       // 地址，最大支持64-bit
using Data_t    = std::vector<uint8_t>;  // 数据按byte存储，支持任意burst长度
using ByteEn_t  = std::vector<bool>;     // 按byte粒度的写使能
using MstId_t   = uint32_t;       // Master ID
using TxnId_t   = uint64_t;       // 全局唯一事务ID
using Timestamp_t = uint64_t;     // 仿真时间戳（单位：ns or cycle）
using SeqNum_t  = uint64_t;       // 全局递增序列号

// ---------- CHI 协议常量 ----------
constexpr uint32_t CHI_FLIT_BYTES = 32;   // 256-bit data flit = 32 bytes
constexpr uint32_t CHI_MAX_FLITS  = 4;    // 最多4笔 data flit (secvec 4-bit)
constexpr uint32_t CHI_CL_BYTES   = 128;  // 1024-bit cacheline = 128 bytes

// CHI 请求 opcode (4-bit REQOPCODE)
enum class ChiReqOpcode : uint8_t {
    ReadNoSnp      = 0x1,
    WriteNoSnpFull = 0xC,
    WriteNoSnpPtl  = 0xD,
    UNKNOWN        = 0xFF
};

inline bool chi_is_read_opcode(int opcode) {
    return opcode == static_cast<int>(ChiReqOpcode::ReadNoSnp);
}
inline bool chi_is_write_opcode(int opcode) {
    return opcode == static_cast<int>(ChiReqOpcode::WriteNoSnpFull) ||
           opcode == static_cast<int>(ChiReqOpcode::WriteNoSnpPtl);
}

// 从 secvec 计算有效 flit 数 (popcount)
// secvec only valid in [3:0]
inline uint32_t secvec_to_flits(uint32_t secvec) {
    return __builtin_popcount(secvec & 0xF);
}

// 从 secvec bit-i 计算该段在 cacheline 中的字节起始偏移
inline uint32_t secvec_bit_to_offset(int bit) {
    return static_cast<uint32_t>(bit) * CHI_FLIT_BYTES;
}

// dataid[1:0] → 字节偏移
inline uint32_t dataid_to_offset(uint32_t dataid) {
    return (dataid & 0x3u) * CHI_FLIT_BYTES;
}

// ---------- 事务类型枚举 ----------
enum class TxnType : uint8_t {
    READ       = 0,
    WRITE      = 1,
    // 扩展预留
    READ_ONCE  = 2,   // CHI ReadOnce
    WRITE_BACK = 3,   // CHI WriteBack
    ATOMIC     = 4,
    UNKNOWN    = 0xFF
};

// ---------- 事务状态枚举 ----------
enum class TxnStatus : uint8_t {
    ISSUED,        // 已发出请求 (Request phase)
    DATA_PHASE,    // 数据传输中 (适用于burst)
    COMPLETED,     // 已完成 (Response received)
    ERROR          // 异常
};

// ---------- 检查结果 ----------
enum class CheckResult : uint8_t {
    PASS,
    FAIL_DATA_MISMATCH,       // 读出数据与预期不一致
    FAIL_NO_PRIOR_WRITE,      // 读了一个从未写过的地址
    WARN_READ_DURING_OVERLAP, // 读发生在多个写overlap期间
    PASS_WITH_RELAXED_ORDER   // 在宽松排序模型下pass
};

// ---------- 报错严重等级 ----------
enum class Severity : uint8_t {
    INFO,
    WARNING,
    ERROR,
    FATAL
};

// 地址对齐辅助 (1024-bit / 128-byte cacheline)
constexpr Addr_t CACHELINE_SIZE  = 128;
constexpr Addr_t CACHELINE_MASK  = ~(CACHELINE_SIZE - 1);
inline Addr_t align_to_cacheline(Addr_t addr) { return addr & CACHELINE_MASK; }
