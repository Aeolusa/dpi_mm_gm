// ============================================================
// file: include/chi_transaction_manager.h
// CHI 事务管理器
//   - 以 (mst_idx, txnid) 作为事务匹配键
//   - 支持 secvec 驱动的有效字节范围
//   - 支持 dataid 驱动的数据组装
//   - SM 写事务由 BFM 参数 IS_SM 标识，使用 secvec popcount 决定 flit 数
//   - 非 SM 事务使用 size 编码决定 flit 数
// ============================================================
#pragma once

#include "types.h"
#include "transaction.h"
#include "consistency_check.h"

#include <unordered_map>
#include <array>
#include <cstdint>
#include <vector>
#include <mutex>

// ============================================================
// MstKey：单个BFM实例内的事务唯一键
//   mst_idx : BFM实例编号 (例: SM0=0, SM1=1, ..., HST=8, TS=9, BLIT=10)
//   txn_id  : 该BFM内的 txnid
// ============================================================
struct MstKey {
    uint32_t mst_idx;
    uint32_t txn_id;

    bool operator==(const MstKey& other) const {
        return mst_idx == other.mst_idx && txn_id == other.txn_id;
    }
};

namespace std {
    template <>
    struct hash<MstKey> {
        size_t operator()(const MstKey& k) const {
            return hash<uint64_t>()(
                (static_cast<uint64_t>(k.mst_idx) << 32) | k.txn_id
            );
        }
    };
}

// ============================================================
// 未完成的写请求记录
//   - 创建于 txreq (WrNoSnp)
//   - dbid 赋值于 rxrsp (DBIDResp/CompDBIDResp)
//   - 数据累积于 txdat (NCBWrData)，每笔按 dataid 放置
// ============================================================
struct ChiOutstandingWrite {
    MstKey   key;           // {mst_idx, txn_id}
    Addr_t   addr;          // cacheline 对齐地址
    uint32_t opcode;        // 写 opcode (WriteNoSnpFull/Ptl)
    uint32_t secvec;        // 4-bit：哪些 32B 段有效
    bool     is_sm = false; // 是否为 SM 类型 master

    // DBID 阶段（rxrsp 后填充）
    uint32_t dbid        = 0;
    bool     dbid_valid  = false;

    // 128字节数据缓冲（按 dataid 散落填入）
    std::array<uint8_t, CHI_CL_BYTES> data_buf  = {};
    std::array<bool,    CHI_CL_BYTES> be_buf     = {};

    // flit 计数
    //   SM 写: expected_flits 初始由 secvec 估算，收到首笔 txdat 后由 data_cnt 覆盖
    //   非 SM: expected_flits = size_to_flits(size)
    uint32_t expected_flits    = 0;
    uint32_t accumulated_flits = 0;
    bool     data_cnt_applied  = false; // SM: data_cnt 已覆盖 expected_flits

    Timestamp_t req_time = 0;
};

// ============================================================
// 未完成的读请求记录
//   - 创建于 txreq (RdNoSnp)
//   - 数据累积于 rxdat (CompData)，每笔按 dataid 放置
// ============================================================
struct ChiOutstandingRead {
    MstKey   key;
    Addr_t   addr;
    uint32_t opcode;
    uint32_t secvec;

    // 128字节数据缓冲
    std::array<uint8_t, CHI_CL_BYTES> data_buf  = {};
    std::array<bool,    CHI_CL_BYTES> be_buf     = {};

    uint32_t expected_flits    = 0;
    uint32_t accumulated_flits = 0;

    Timestamp_t req_time = 0;
};

// ============================================================
// ChiTransactionManager
// ============================================================
class ChiTransactionManager {
public:
    explicit ChiTransactionManager(bool strict_mode = true);

    // ---- DPI-C 接口 ----

    // txreq 通道：读或写请求（由 opcode 区分）
    //   is_sm: 是否为 SM 类型 master（由 BFM 参数传入）
    void process_txreq(uint32_t mst_idx, uint32_t txn_id,
                       Addr_t addr, uint32_t size, uint32_t opcode,
                       uint32_t secvec, Timestamp_t req_time,
                       bool is_sm);

    // rxrsp 通道：DBIDResp / CompDBIDResp
    void process_rxrsp_dbid(uint32_t mst_idx, uint32_t txn_id,
                            uint32_t opcode, uint32_t dbid);

    // txdat 通道：NCBWrData（写数据）
    //   txn_id 这里实际传入的是 dbid 值！
    void process_txdat(uint32_t mst_idx, uint32_t txn_id_as_dbid,
                       uint32_t opcode, uint32_t dataid,
                       const uint8_t* flit_data,  // 32 bytes
                       uint32_t be_mask,           // 32-bit byte enable
                       uint32_t data_cnt);

    // rxdat 通道：CompData（读返回数据）
    void process_rxdat(uint32_t mst_idx, uint32_t txn_id,
                       uint32_t opcode, uint32_t dataid,
                       const uint8_t* flit_data,  // 32 bytes
                       uint32_t be_mask,
                       uint32_t data_cnt);

    // 统计引用
    ConsistencyChecker& get_checker() { return checker_; }

private:
    ConsistencyChecker checker_;
    SeqNum_t           global_seq_;

    // 线程安全锁（多通道 DPI 并发调用保护）
    std::mutex         mtx_;

    // outstanding_writes_: (mst_idx, txnid) → 写请求
    std::unordered_map<MstKey, ChiOutstandingWrite> outstanding_writes_;

    // outstanding_reads_: (mst_idx, txnid) → 读请求
    std::unordered_map<MstKey, ChiOutstandingRead>  outstanding_reads_;

    // dbid 逆向映射: {mst_idx, dbid} → 原始 txnid
    // 用于 txdat 时由 dbid 找回写请求
    std::unordered_map<MstKey, uint32_t> dbid_to_txnid_;

    // 内部辅助：从数据缓冲构建 Transaction 并提交
    void finalize_write(ChiOutstandingWrite& req, Timestamp_t resp_time);
    void finalize_read (ChiOutstandingRead&  req, Timestamp_t resp_time);
};
