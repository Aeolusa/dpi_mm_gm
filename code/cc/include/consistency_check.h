// ============================================================
// file: consistency_checker.h
// 核心检查逻辑：
//   1. WRITE完成时 → 更新ShadowMemory
//   2. READ完成时  → 与ShadowMemory比对
//   3. Overlap检测 → 允许读到overlap窗口内任一值
// ============================================================
#pragma once
#include "types.h"
#include "transaction.h"
#include "shadow_memory.h"

struct CheckReport {
    CheckResult   result;
    Severity      severity;
    Transaction   txn;            // 触发检查的事务
    Addr_t        fail_addr;      // 出错byte地址
    uint8_t       expected;
    uint8_t       actual;
    std::vector<WriteRecord> related_writes; // 相关写记录
    std::string   message;
};

class ConsistencyChecker {
public:
    explicit ConsistencyChecker(bool strict_mode = true)
        : strict_mode_(strict_mode) {}

    // ---------- 处理写完成 ----------
    void process_write(const Transaction& txn) {
        assert(txn.type == TxnType::WRITE);
        assert(txn.status == TxnStatus::COMPLETED);

        shadow_mem_.write(txn.addr, txn.data, txn.byte_enable, txn);

        // 记录到活跃写集合（用于overlap检测）
        track_active_write(txn);

        stats_.total_writes++;
    }

    // ---------- 处理读完成并检查 ----------
    CheckReport process_read(const Transaction& txn) {
        assert(txn.type == TxnType::READ);
        assert(txn.status == TxnStatus::COMPLETED);

        stats_.total_reads++;

        uint32_t total_bytes = txn.size * txn.burst_len;
        CheckReport report;
        report.txn    = txn;
        report.result = CheckResult::PASS;

        for (uint32_t i = 0; i < total_bytes; ++i) {
            Addr_t byte_addr = txn.addr + i;
            uint8_t actual_val = txn.data[i];

            // === 场景1：该地址从未被写过 ===
            if (!shadow_mem_.has_been_written(byte_addr)) {
                if (strict_mode_ && actual_val != 0x00) {
                    report.result    = CheckResult::FAIL_NO_PRIOR_WRITE;
                    report.severity  = Severity::ERROR;
                    report.fail_addr = byte_addr;
                    report.expected  = 0x00;
                    report.actual    = actual_val;
                    report.message   = format_error(report);
                    stats_.errors++;
                    return report;
                }
                continue;
            }

            // === 场景2：检查是否有overlap写 ===
            auto overlap_writes = find_overlapping_writes(byte_addr, txn.req_time);
            if (!overlap_writes.empty()) {
                // 宽松检查：读值必须是某一次写的值
                auto possible = shadow_mem_.get_possible_values(
                    byte_addr,
                    txn.req_time - overlap_window_,
                    txn.resp_time
                );
                bool found = std::find(possible.begin(), possible.end(),
                                       actual_val) != possible.end();
                if (!found) {
                    report.result        = CheckResult::FAIL_DATA_MISMATCH;
                    report.severity      = Severity::ERROR;
                    report.fail_addr     = byte_addr;
                    report.expected      = possible.empty() ? 0 : possible.front();
                    report.actual        = actual_val;
                    report.related_writes = {};
                    report.message       = format_error(report);
                    stats_.errors++;
                    return report;
                }
                continue;
            }

            // === 场景3：正常比对 ===
            auto expected_opt = shadow_mem_.read_byte(byte_addr);
            uint8_t expected_val = expected_opt.value_or(0x00);

            if (actual_val != expected_val) {
                report.result    = CheckResult::FAIL_DATA_MISMATCH;
                report.severity  = Severity::ERROR;
                report.fail_addr = byte_addr;
                report.expected  = expected_val;
                report.actual    = actual_val;
                // 附上写历史帮助debug
                const auto& hist = shadow_mem_.get_write_history(byte_addr);
                report.related_writes.assign(hist.begin(), hist.end());
                report.message = format_error(report);
                stats_.errors++;
                return report;
            }
        }

        stats_.passes++;
        return report;
    }

    // ---------- 统计 ----------
    struct Stats {
        uint64_t total_reads  = 0;
        uint64_t total_writes = 0;
        uint64_t passes       = 0;
        uint64_t errors       = 0;
        uint64_t warnings     = 0;
    };
    const Stats& get_stats() const { return stats_; }

private:
    ShadowMemory shadow_mem_;
    bool         strict_mode_;
    Timestamp_t  overlap_window_ = 10; // 可配置的overlap检测时间窗口
    Stats        stats_;

    // 活跃写追踪 (addr -> 最近完成的写事务列表)
    std::unordered_map<Addr_t, std::deque<Transaction>> active_writes_;

    void track_active_write(const Transaction& txn) {
        uint32_t total_bytes = txn.size * txn.burst_len;
        for (uint32_t i = 0; i < total_bytes; ++i) {
            if (txn.byte_en_at(i)) {
                auto& q = active_writes_[txn.addr + i];
                q.push_back(txn);
                while (q.size() > 4) q.pop_front();
            }
        }
    }

    std::vector<Transaction> find_overlapping_writes(
        Addr_t addr, Timestamp_t read_time) const
    {
        std::vector<Transaction> result;
        auto it = active_writes_.find(addr);
        if (it == active_writes_.end()) return result;

        for (const auto& wr : it->second) {
            // 写的completion与读的request在时间上重叠
            if (wr.resp_time >= read_time - overlap_window_ &&
                wr.req_time  <= read_time) {
                result.push_back(wr);
            }
        }
        return result;
    }

    std::string format_error(const CheckReport& rpt) const {
        std::ostringstream oss;
        oss << "CONSISTENCY CHECK FAILED!\n"
            << "  Transaction : " << rpt.txn.to_string() << "\n"
            << "  Fail Address: 0x" << std::hex << rpt.fail_addr << "\n"
            << "  Expected    : 0x" << std::setw(2) << std::setfill('0')
            << (int)rpt.expected << "\n"
            << "  Actual      : 0x" << std::setw(2) << std::setfill('0')
            << (int)rpt.actual << "\n"
            << "  Write History:";
        for (const auto& wr : rpt.related_writes) {
            oss << "\n    [SEQ=" << std::dec << wr.global_seq
                << " MST" << wr.master_id
                << " TXN#" << wr.txn_id
                << " val=0x" << std::hex << (int)wr.value
                << " t=" << std::dec << wr.write_time << "]";
        }
        return oss.str();
    }
};