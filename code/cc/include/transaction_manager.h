// ============================================================
// file: transaction_manager.h
// 职责：
//   1. 从DPI接口接收原始事务
//   2. 分配全局序列号，维护per-master队列
//   3. 按completion顺序派发给Checker
// ============================================================
#pragma once
#include "types.h"
#include "transaction.h"
#include "consistency_checker.h"

#include <queue>
#include <iostream>

class TransactionManager {
public:
    TransactionManager(uint32_t num_masters, bool strict = true)
        : num_masters_(num_masters),
          checker_(strict),
          global_seq_(0),
          txn_id_counter_(0)
    {
        pending_txns_.resize(num_masters);
    }

    // ---- 阶段1：事务请求 (request phase) ----
    TxnId_t submit_request(MstId_t      master_id,
                           TxnType      type,
                           Addr_t       addr,
                           uint32_t     size,
                           uint32_t     burst_len,
                           const Data_t& data,        // WRITE: 写数据; READ: 空
                           const ByteEn_t& byte_en,
                           Timestamp_t  req_time)
    {
        Transaction txn;
        txn.txn_id     = txn_id_counter_++;
        txn.global_seq = 0;  // completion时分配
        txn.master_id  = master_id;
        txn.type       = type;
        txn.addr       = addr;
        txn.size       = size;
        txn.burst_len  = burst_len;
        txn.data       = data;
        txn.byte_enable = byte_en;
        txn.req_time   = req_time;
        txn.resp_time  = 0;
        txn.status     = TxnStatus::ISSUED;

        pending_txns_[master_id][txn.txn_id] = txn;
        return txn.txn_id;
    }

    // ---- 阶段2：事务完成 (completion/response phase) ----
    CheckReport complete_transaction(MstId_t     master_id,
                                     TxnId_t     txn_id,
                                     const Data_t& resp_data, // READ: 读返回数据
                                     Timestamp_t  resp_time)
    {
        auto& pending = pending_txns_[master_id];
        auto it = pending.find(txn_id);
        assert(it != pending.end() && "TXN ID not found in pending");

        Transaction txn = it->second;
        txn.resp_time  = resp_time;
        txn.global_seq = global_seq_++;  // 按completion顺序编号
        txn.status     = TxnStatus::COMPLETED;

        if (txn.type == TxnType::READ) {
            txn.data = resp_data;  // 填入读返回数据
        }

        pending.erase(it);

        // ---- 派发给Checker ----
        CheckReport report;
        if (txn.type == TxnType::WRITE) {
            checker_.process_write(txn);
            report.result   = CheckResult::PASS;
            report.txn      = txn;
            report.severity = Severity::INFO;
        } else {
            report = checker_.process_read(txn);
            if (report.result != CheckResult::PASS) {
                std::cerr << "\n========== CONSISTENCY ERROR ==========\n"
                          << report.message
                          << "\n=======================================\n";
            }
        }

        // 归档
        completed_txns_.push_back(txn);
        return report;
    }

    // ---- 仿真结束汇总 ----
    void final_report() const {
        auto& s = checker_.get_stats();
        std::cout << "\n╔═══════════════════════════════════════╗\n"
                  << "║   Memory Consistency Check Summary    ║\n"
                  << "╠═══════════════════════════════════════╣\n"
                  << "║  Total Writes  : " << std::setw(18) << s.total_writes << " ║\n"
                  << "║  Total Reads   : " << std::setw(18) << s.total_reads  << " ║\n"
                  << "║  PASS          : " << std::setw(18) << s.passes       << " ║\n"
                  << "║  ERRORS        : " << std::setw(18) << s.errors       << " ║\n"
                  << "╚═══════════════════════════════════════╝\n";
        if (s.errors > 0) {
            std::cerr << "*** TEST FAILED: " << s.errors
                      << " consistency violation(s) detected ***\n";
        } else {
            std::cout << "*** TEST PASSED ***\n";
        }
    }

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