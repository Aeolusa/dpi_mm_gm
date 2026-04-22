// ============================================================
// file: include/shadow_memory.h
// ============================================================
#pragma once
#include "types.h"
#include "transaction.h"

#include <deque>
#include <vector>
#include <unordered_map>
#include <optional>
#include <mutex>
#include <functional>
#include <string>

// ---- 单Byte写记录 ----
struct WriteRecord {
    uint8_t      value;
    MstId_t      master_id;
    TxnId_t      txn_id;
    SeqNum_t     global_seq;
    Timestamp_t  write_time;      // completion time

    std::string  to_string() const;
};

// ---- 单Byte元信息（聚合当前值 + 写历史） ----
struct ByteSlot {
    uint8_t                  current_value = 0x00;
    bool                     initialized   = false;  // 是否被写过
    std::deque<WriteRecord>  write_history;
};

// ---- 配置结构体 ----
struct ShadowMemoryConfig {
    size_t      max_write_history   = 16;      // 每byte保留写历史深度
    uint8_t     default_init_value  = 0x00;    // 未初始化地址的默认读值
    bool        track_read_log      = false;   // 是否记录读操作日志
    uint64_t    addr_space_limit    = 0;       // 0=无限制; 非0=最大可访问地址
    // 以Page(4KB)为单位的稀疏分配
    uint32_t    page_size_bits      = 12;      // 4KB pages
};

// ---- 读日志记录（可选功能） ----
struct ReadLogEntry {
    Addr_t       addr;
    uint8_t      value;
    MstId_t      reader_master_id;
    TxnId_t      reader_txn_id;
    Timestamp_t  read_time;
};

// ============================================================
// ShadowMemory 类声明
// ============================================================
class ShadowMemory {
public:
    // ---------- 构造/析构 ----------
    explicit ShadowMemory(const ShadowMemoryConfig& cfg = {});
    ~ShadowMemory();

    // 禁止拷贝，允许移动
    ShadowMemory(const ShadowMemory&)            = delete;
    ShadowMemory& operator=(const ShadowMemory&) = delete;
    ShadowMemory(ShadowMemory&&)                 = default;
    ShadowMemory& operator=(ShadowMemory&&)      = default;

    // ---------- 写操作 ----------
    // 整笔事务写入
    void write(const Transaction& txn);

    // 细粒度：指定地址范围写入
    void write(Addr_t       base_addr,
               const Data_t&   data,
               const ByteEn_t& byte_en,
               MstId_t      master_id,
               TxnId_t      txn_id,
               SeqNum_t     global_seq,
               Timestamp_t  write_time);

    // 单byte写入
    void write_byte(Addr_t      addr,
                    uint8_t     value,
                    MstId_t     master_id,
                    TxnId_t     txn_id,
                    SeqNum_t    global_seq,
                    Timestamp_t write_time);

    // ---------- 读操作 ----------
    // 读取当前值（单byte）
    std::optional<uint8_t> read_byte(Addr_t addr) const;

    // 读取连续区域
    Data_t read(Addr_t base_addr, uint32_t len) const;

    // 读取并记录日志（关联读事务信息）
    Data_t read_and_log(Addr_t      base_addr,
                        uint32_t    len,
                        MstId_t     reader_master_id,
                        TxnId_t     reader_txn_id,
                        Timestamp_t read_time);

    // ---------- 写历史查询 ----------
    // 获取某地址的完整写历史
    const std::deque<WriteRecord>& get_write_history(Addr_t addr) const;

    // 获取某地址最后一次写记录
    std::optional<WriteRecord> get_last_write(Addr_t addr) const;

    // 获取某地址在时间窗口内的所有可能值
    std::vector<uint8_t> get_possible_values(Addr_t      addr,
                                             Timestamp_t window_start,
                                             Timestamp_t window_end) const;

    // 获取某地址在某时刻之前最新的值
    std::optional<uint8_t> get_value_at_time(Addr_t addr, Timestamp_t time) const;

    // ---------- 状态查询 ----------
    bool     has_been_written(Addr_t addr) const;
    bool     is_range_written(Addr_t base_addr, uint32_t len) const;
    uint64_t get_total_written_bytes() const;
    uint64_t get_total_pages_allocated() const;

    // ---------- 区域操作 ----------
    // 预初始化一段内存（模拟bootloader/firmware预置数据）
    void preload(Addr_t base_addr, const Data_t& data);

    // 清除某地址范围的内容和历史
    void invalidate_range(Addr_t base_addr, uint32_t len);

    // 全部清空
    void reset();

    // ---------- 调试/Dump ----------
    // dump某地址范围的当前值（hex格式）
    std::string dump_range(Addr_t base_addr, uint32_t len) const;

    // dump某地址的写历史
    std::string dump_write_history(Addr_t addr) const;

    // dump统计信息
    std::string dump_stats() const;

    // 遍历所有已写byte（用于post-sim分析）
    void for_each_written_byte(
        const std::function<void(Addr_t, const ByteSlot&)>& visitor) const;

    // ---------- 配置访问 ----------
    const ShadowMemoryConfig& get_config() const { return config_; }

private:
    // ---- Page-based 稀疏存储 ----
    // 一级索引：page_id -> page内容
    // page_id = addr >> page_size_bits
    // page内offset = addr & ((1 << page_size_bits) - 1)
    struct Page {
        std::vector<ByteSlot> slots;   // 固定大小 = 1 << page_size_bits
        bool any_written = false;
    };

    using PageId_t = uint64_t;

    PageId_t addr_to_page_id(Addr_t addr) const;
    uint32_t addr_to_page_offset(Addr_t addr) const;

    // 获取或创建Page
    Page& get_or_create_page(PageId_t page_id);

    // 只读获取Page（不创建）
    const Page* get_page(PageId_t page_id) const;

    // 获取ByteSlot引用（写路径）
    ByteSlot& get_or_create_slot(Addr_t addr);

    // 只读获取ByteSlot（读路径）
    const ByteSlot* get_slot(Addr_t addr) const;

    // 地址合法性检查
    bool validate_addr(Addr_t addr) const;

    // ---- 成员变量 ----
    ShadowMemoryConfig config_;
    uint32_t           page_size_;
    uint32_t           page_size_mask_;
    std::unordered_map<PageId_t, Page> pages_;

    // 统计
    uint64_t total_write_ops_  = 0;
    uint64_t total_read_ops_   = 0;
    uint64_t total_bytes_written_ = 0;

    // 读日志（可选）
    std::vector<ReadLogEntry> read_log_;

    // 空写历史（用于返回const引用）
    static const std::deque<WriteRecord> empty_history_;

    // 线程安全锁（多master并发调用时可选启用）
    mutable std::mutex mtx_;
};