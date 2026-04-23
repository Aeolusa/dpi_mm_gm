// ============================================================
// file: include/chi_transaction_manager.h
// ============================================================
#pragma once

#include "types.h"
#include "transaction.h"
#include "consistency_check.h"

#include <unordered_map>
#include <cstdint>
#include <vector>

// 提取的未完成CHI请求记录
struct ChiOutstandingWrite {
    uint32_t src_id;
    uint32_t tgt_id;
    TxnId_t  txn_id;
    Addr_t   addr;
    uint32_t size;
    uint32_t burst_len;
    
    // CHI特定
    uint32_t dbid;
    bool     dbid_valid = false;
    
    // 累积的数据和byte enable
    Data_t   data;
    ByteEn_t byte_enable;
    
    Timestamp_t req_time;
    uint32_t accumulated_bytes = 0;
};

// 哈希键用于查找表项 (src_id, tgt_id, txn_id)
struct TxnKey {
    uint32_t src_id;
    uint32_t tgt_id;
    TxnId_t  txn_id;

    bool operator==(const TxnKey& other) const {
        return src_id == other.src_id && tgt_id == other.tgt_id && txn_id == other.txn_id;
    }
};

namespace std {
    template <>
    struct hash<TxnKey> {
        size_t operator()(const TxnKey& k) const {
            return ((hash<uint32_t>()(k.src_id) ^
                    (hash<uint32_t>()(k.tgt_id) << 1)) >> 1) ^
                   (hash<TxnId_t>()(k.txn_id) << 1);
        }
    };
}

class ChiTransactionManager {
public:
    ChiTransactionManager(bool strict_mode = true);

    // ---- DPI-C 接口映射 ----
    // txreq通道: 发起写请求
    void process_txreq_flit(uint32_t src_id, uint32_t tgt_id, uint32_t txn_id,
                            Addr_t addr, uint32_t size, uint32_t burst_len,
                            Timestamp_t req_time);

    // rxrsp通道: 收到 DBIDResp / CompDBIDResp 等
    void process_rxrsp_flit(uint32_t src_id, uint32_t tgt_id, uint32_t txn_id,
                            uint32_t dbid);

    // txdat通道: 收到写数据 NCBWrdata
    void process_txdat_flit(uint32_t src_id, uint32_t tgt_id, uint32_t dbid,
                            const Data_t& data_flit, const ByteEn_t& be_flit,
                            uint32_t offset, Timestamp_t resp_time);

    // 读请求的简化接口
    void process_read_completed(uint32_t src_id, uint32_t tgt_id, uint32_t txn_id,
                                Addr_t addr, uint32_t size, uint32_t burst_len,
                                const Data_t& data, Timestamp_t req_time, Timestamp_t resp_time);

    // 统计和检查器引用
    ConsistencyChecker& get_checker() { return checker_; }
    
private:
    ConsistencyChecker checker_;
    SeqNum_t           global_seq_;
    
    // 未完成的写请求表: (src, tgt, txn) -> Request
    std::unordered_map<TxnKey, ChiOutstandingWrite> outstanding_writes_;
    
    // DBID 映射辅助: (src, tgt, dbid) -> TxnKey
    // 用于 txdat 时通过 DBID 找回原始的 TxnKey
    std::unordered_map<TxnKey, TxnKey> dbid_to_txnkey_;
};
