// ============================================================
// file: src/consistency_check.cc
// ============================================================
#include "consistency_check.h"

#include <iostream>
#include <iomanip>
#include <cassert>
#include <algorithm>

void ConsistencyChecker::process_write(const Transaction& txn) {
    assert(txn.type == TxnType::WRITE);
    assert(txn.status == TxnStatus::COMPLETED);

    shadow_mem_.write(txn);

    // 记录到活跃写集合（用于overlap检测）
    track_active_write(txn);

    stats_.total_writes++;
}

CheckReport ConsistencyChecker::process_read(const Transaction& txn) {
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

void ConsistencyChecker::track_active_write(const Transaction& txn) {
    uint32_t total_bytes = txn.size * txn.burst_len;
    for (uint32_t i = 0; i < total_bytes; i += BLOCK_SIZE) {
        // Just track the block addresses covered by this write. 
        // Note: For partial blocks, any write to the block registers as a block write.
        Addr_t block_addr = (txn.addr + i) & ~(static_cast<Addr_t>(BLOCK_SIZE - 1));
        auto& q = active_writes_[block_addr];
        q.push_back(txn);
        while (q.size() > 4) q.pop_front();
    }
}

std::vector<Transaction> ConsistencyChecker::find_overlapping_writes(
    Addr_t addr, Timestamp_t read_time) const
{
    std::vector<Transaction> result;
    Addr_t block_addr = addr & ~(static_cast<Addr_t>(BLOCK_SIZE - 1));
    auto it = active_writes_.find(block_addr);
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

std::string ConsistencyChecker::format_error(const CheckReport& rpt) const {
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
        oss << "\n    " << wr.to_string();
    }
    return oss.str();
}
