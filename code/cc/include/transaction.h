// ============================================================
// file: transaction.h
// ============================================================
#pragma once
#include "types.h"

struct Transaction {
    // ---- 唯一标识 ----
    TxnId_t     txn_id;          // 全局唯一ID（由RefModel分配）
    SeqNum_t    global_seq;      // 全局序列号，用于排序

    // ---- 来源 ----
    MstId_t     master_id;       // 发起者
    uint32_t    chi_txn_id;      // CHI协议层的TxnID（可选）

    // ---- 事务属性 ----
    TxnType     type;
    Addr_t      addr;            // 起始地址
    uint32_t    size;            // 单拍字节数 (1/2/4/8/16/32/64)
    uint32_t    burst_len;       // Burst长度（AXI len+1）
    Data_t      data;            // 写数据 或 读返回数据
    ByteEn_t    byte_enable;     // 写使能，长度 = size * burst_len

    // ---- 时间信息 ----
    Timestamp_t req_time;        // 请求发出时间
    Timestamp_t resp_time;       // 响应完成时间（completion point）

    // ---- 状态 ----
    TxnStatus   status = TxnStatus::ISSUED;

    // ---- 辅助方法 ----
    Addr_t end_addr() const { return addr + size * burst_len; }

    // 获取某个byte偏移处的写使能
    bool byte_en_at(uint32_t offset) const {
        if (offset < byte_enable.size()) return byte_enable[offset];
        return true;  // 默认全使能
    }

    std::string to_string() const {
        std::ostringstream oss;
        oss << "[TXN#" << txn_id
            << " MST" << master_id
            << " " << (type == TxnType::WRITE ? "WR" : "RD")
            << " @0x" << std::hex << addr
            << " sz=" << std::dec << size
            << " t=" << req_time << "->" << resp_time
            << "]";
        return oss.str();
    }
};