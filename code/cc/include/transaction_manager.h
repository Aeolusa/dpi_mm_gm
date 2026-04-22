// ============================================================
// file: transaction_manager.h
// 职责：
//   1. 从DPI接口接收原始事务
//   2. 分配全局序列号，维护per-master队列
//   3. 按completion顺序派发给Checker
//   4. MstId_t should be corrected to srcid(CHI protocol)
// ============================================================
#pragma once
#include "types.h"
#include "transaction.h"
#include "consistency_check.h"

#include <queue>
#include <iostream>

class TransactionManager {
public:
    TransactionManager(uint32_t num_masters, bool strict = true);

    // ---- 阶段1：事务请求 (request phase) ----
    TxnId_t submit_request(MstId_t      master_id,
                           TxnType      type,
                           Addr_t       addr,
                           uint32_t     size,
                           uint32_t     burst_len,
                           const Data_t& data,        // WRITE: 写数据; READ: 空
                           const ByteEn_t& byte_en,
                           Timestamp_t  req_time);

    // ---- 阶段2：事务完成 (completion/response phase) ----
    CheckReport complete_transaction(MstId_t     master_id,
                                     TxnId_t     txn_id,
                                     const Data_t& resp_data, // READ: 读返回数据
                                     Timestamp_t  resp_time);

    // ---- 仿真结束汇总 ----
    void final_report() const;

private:
    uint32_t    num_masters_;
    ConsistencyChecker checker_;
    SeqNum_t    global_seq_;
    TxnId_t     txn_id_counter_;

    // per-master pending事务: master_id -> {txn_id -> Transaction}
    std::vector<std::unordered_map<TxnId_t, Transaction>> pending_txns_;

    // 已完成事务归档（可选，用于post-sim分析）
    std::vector<Transaction> completed_txns_;
};