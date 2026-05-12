// ============================================================
// file: transaction.h
// ============================================================
#pragma once
#include "types.h"
#include <bitset>

struct Transaction {
    // ---- 唯一标识 ----
    TxnId_t     txn_id;          // CHI txnid
    SeqNum_t    global_seq;      // 全局序列号，用于排序

    // ---- 来源 ----
    uint32_t    src_id;          // BFM 实例编号 (mst_idx)，0=SM0, 1=SM1...
    uint32_t    tgt_id;          // 保留字段，通常为 0
    uint32_t    dbid;            // Data Buffer ID (从 rxrsp 获取)

    // ---- 事务属性 ----
    TxnType     type;
    Addr_t      addr;            // cacheline 对齐起始地址
    uint32_t    size;            // 数据缓冲区字节数（固定 CHI_CL_BYTES=128）
    uint32_t    burst_len;       // 固定为 1（数据由 byte_enable 标记有效字节）
    uint32_t    secvec;          // 4-bit 有效段向量，bit-i 对应 bytes[i*32:(i+1)*32-1]
    Data_t      data;            // 128 字节数据缓冲（按 dataid 组装）
    ByteEn_t    byte_enable;     // 128 位 byte enable（secvec + 每笔 flit be 的组合）

    // ---- 时间信息 ----
    Timestamp_t req_time;        // 请求发出时间
    Timestamp_t resp_time;       // 响应完成时间

    // ---- 状态 ----
    TxnStatus   status = TxnStatus::ISSUED;

    // ---- 辅助方法 ----
    // 有效数据的字节数 = secvec 有效 bit 数 * CHI_FLIT_BYTES
    uint32_t valid_bytes() const { return secvec_to_flits(secvec) * CHI_FLIT_BYTES; }
    Addr_t end_addr() const { return addr + CHI_CL_BYTES; }

    // 获取某个byte偏移处的 byte enable
    bool byte_en_at(uint32_t offset) const {
        if (offset < byte_enable.size()) return byte_enable[offset];
        return false;  // 超出范围视为无效
    }

    std::string to_string() const {
        std::ostringstream oss;
        oss << "[TXN#" << txn_id
            << " MST=" << src_id
            << " " << (type == TxnType::WRITE ? "WR" : "RD")
            << " @0x" << std::hex << addr
            << " secvec=0b" << std::bitset<4>(secvec)
            << " t=" << std::dec << req_time << "->" << resp_time
            << "]";
        return oss.str();
    }
};