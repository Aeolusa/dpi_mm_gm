// ============================================================
// file: consistency_check.h
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
    void process_write(const Transaction& txn);

    // ---------- 处理读完成并检查 ----------
    CheckReport process_read(const Transaction& txn);

    // ---------- 后门预加载 ----------
    // 加载 hex 文件到 shadow memory（不产生写历史）
    // 格式: @ADDR (hex地址)，后续每行为 hex 数据，宽度 = mem_width_bytes 字节
    // 支持可变 memory 宽度（如 4/8/16/32 字节）
    bool preload_hex_file(const std::string& filepath, uint32_t mem_width_bytes);

    // 直接 preload 一段数据
    void preload(Addr_t base_addr, const Data_t& data);

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

    void track_active_write(const Transaction& txn);
    std::vector<Transaction> find_overlapping_writes(Addr_t addr, Timestamp_t read_time) const;
    std::string format_error(const CheckReport& rpt) const;
};