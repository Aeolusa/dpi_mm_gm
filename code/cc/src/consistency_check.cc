// ============================================================
// file: src/consistency_check.cc
// ============================================================
#include "consistency_check.h"
#include "logger.h"

#include <iostream>
#include <iomanip>
#include <cassert>
#include <algorithm>
#include <fstream>
#include <sstream>

// ============================================================
// 后门预加载：hex 文件 → shadow memory
// 格式:
//   @80000000              ← 16进制基地址
//   ffffffffaaaaaaaa       ← 每行 mem_width_bytes 字节，hex 编码
//   ...
// 数据按地址递增存储，每行 hex 字符数 = mem_width_bytes * 2
// hex 串高位在左，内存存储按小端（低字节在低地址）
// ============================================================
bool ConsistencyChecker::preload_hex_file(const std::string& filepath,
                                           uint32_t mem_width_bytes)
{
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
        LOG_ERROR("[PRELOAD] Cannot open hex file: " << filepath << "\n");
        return false;
    }

    Addr_t base_addr = 0;
    bool   addr_set  = false;
    uint64_t total_bytes = 0;
    uint64_t line_no = 0;

    std::string line;
    while (std::getline(ifs, line)) {
        line_no++;

        // 去除首尾空白
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue; // 空行
        line = line.substr(start);
        size_t end = line.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) line = line.substr(0, end + 1);

        if (line.empty()) continue;

        // 跳过注释行（以 // 或 # 开头）
        if (line[0] == '#' || (line.size() >= 2 && line[0] == '/' && line[1] == '/'))
            continue;

        // @ADDR 行
        if (line[0] == '@') {
            std::string addr_str = line.substr(1);
            base_addr = std::stoull(addr_str, nullptr, 16);
            addr_set = true;
            continue;
        }

        if (!addr_set) {
            LOG_ERROR("[PRELOAD] Data before @ADDR at line " << line_no << "\n");
            continue;
        }

        // 数据行：解析 hex 字符串为字节数组
        // hex 串高位在左 → 先出现的 hex 字符是高字节
        // 存储到内存时按小端：高字节放高地址
        uint32_t hex_len = static_cast<uint32_t>(line.size());
        uint32_t byte_count = hex_len / 2;

        if (byte_count != mem_width_bytes) {
            LOG_DEBUG("[PRELOAD] Line " << line_no << ": expected "
                      << mem_width_bytes * 2 << " hex chars, got " << hex_len
                      << ", adjusting\n");
        }

        Data_t data(byte_count, 0);
        for (uint32_t i = 0; i < byte_count; ++i) {
            // hex 串位置 i*2 对应最高字节
            // 小端存储：hex 位置 i*2 → data[byte_count - 1 - i]
            std::string byte_str = line.substr(i * 2, 2);
            uint8_t val = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
            data[byte_count - 1 - i] = val;
        }

        shadow_mem_.preload(base_addr, data);
        base_addr += byte_count;
        total_bytes += byte_count;
    }

    LOG_ALWAYS("[PRELOAD] Loaded " << total_bytes << " bytes from " << filepath << "\n");
    return true;
}

void ConsistencyChecker::preload(Addr_t base_addr, const Data_t& data) {
    shadow_mem_.preload(base_addr, data);
}


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

    // 固定遍历128字节缓冲，但只检查 byte_enable=true 的字节
    // （即 secvec 指示的有效段内且 per-flit be 有效的字节）
    uint32_t total_bytes = static_cast<uint32_t>(txn.data.size());
    CheckReport report;
    report.txn    = txn;
    report.result = CheckResult::PASS;

    for (uint32_t i = 0; i < total_bytes; ++i) {
        // 跳过 secvec 未选中的段（byte_enable 已在 finalize_read 中按 secvec+be 设置）
        if (!txn.byte_en_at(i)) continue;

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
    // 遍历 128 字节缓冲，按 BLOCK_SIZE(32B) 记录写活跃集合
    for (uint32_t i = 0; i < CHI_CL_BYTES; i += BLOCK_SIZE) {
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
