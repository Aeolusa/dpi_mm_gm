// ============================================================
// file: src/transaction_manager.cc
// ============================================================
#include "transaction_manager.h"
#include "logger.h"
#include <iomanip>

TransactionManager::TransactionManager(uint32_t num_masters, bool strict)
    : num_masters_(num_masters),
      checker_(strict),
      global_seq_(0),
      txn_id_counter_(0)
{
    pending_txns_.resize(num_masters);
}

TxnId_t TransactionManager::submit_request(MstId_t      master_id,
                                           TxnType      type,
                                           Addr_t       addr,
                                           uint32_t     size,
                                           uint32_t     burst_len,
                                           const Data_t& data,
                                           const ByteEn_t& byte_en,
                                           Timestamp_t  req_time)
{
    Transaction txn;
    txn.txn_id     = txn_id_counter_++;
    txn.global_seq = 0;  // completion时分配
    txn.src_id     = master_id;  // mst_idx (BFM实例编号)
    txn.tgt_id     = 0;
    txn.dbid       = 0;
    txn.type       = type;
    txn.addr       = addr;
    txn.size       = size;
    txn.burst_len  = burst_len;
    txn.secvec     = 0;  // TransactionManager不使用secvec，默认全有效
    txn.data       = data;
    txn.byte_enable = byte_en;
    txn.req_time   = req_time;
    txn.resp_time  = 0;
    txn.status     = TxnStatus::ISSUED;

    pending_txns_[master_id][txn.txn_id] = txn;
    return txn.txn_id;
}

CheckReport TransactionManager::complete_transaction(MstId_t     master_id,
                                                     TxnId_t     txn_id,
                                                     const Data_t& resp_data,
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
            LOG_ERROR("\n========== CONSISTENCY ERROR ==========\n"
                      << report.message
                      << "\n=======================================\n");
        }
    }

    // 归档
    completed_txns_.push_back(txn);
    return report;
}

void TransactionManager::final_report() const {
    auto& s = checker_.get_stats();
    std::ostringstream oss;
    oss << "\n╔═══════════════════════════════════════╗\n"
        << "║   Memory Consistency Check Summary    ║\n"
        << "╠═══════════════════════════════════════╣\n"
        << "║  Total Writes  : " << std::setw(18) << s.total_writes << " ║\n"
        << "║  Total Reads   : " << std::setw(18) << s.total_reads  << " ║\n"
        << "║  PASS          : " << std::setw(18) << s.passes       << " ║\n"
        << "║  ERRORS        : " << std::setw(18) << s.errors       << " ║\n"
        << "╚═══════════════════════════════════════╝\n";
    if (s.errors > 0) {
        oss << "*** TEST FAILED: " << s.errors
            << " consistency violation(s) detected ***\n";
    } else {
        oss << "*** TEST PASSED ***\n";
    }
    LOG_ALWAYS(oss.str());
}
