// ============================================================
// file: src/shadow_memory.cpp
// ============================================================

#include "shadow_memory.h"

#include <cassert>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <cstring>

// ============================================================
// 静态成员初始化
// ============================================================
const std::deque<WriteRecord> ShadowMemory::empty_history_;

// ============================================================
// WriteRecord::to_string
// ============================================================
std::string WriteRecord::to_string() const {
    std::ostringstream oss;
    oss << "[WR SEQ=" << global_seq
        << " MST=" << master_id
        << " TXN=#" << txn_id
        << " val=0x" << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(value)
        << " t=" << std::dec << write_time
        << "]";
    return oss.str();
}

// ============================================================
// 构造 / 析构
// ============================================================
ShadowMemory::ShadowMemory(const ShadowMemoryConfig& cfg)
    : config_(cfg)
{
    // 校验page_size_bits合理性 (4~20, 即16B~1MB page)
    if (config_.page_size_bits < 4 || config_.page_size_bits > 20) {
        throw std::invalid_argument(
            "ShadowMemory: page_size_bits must be in [4, 20], got "
            + std::to_string(config_.page_size_bits));
    }

    if (config_.max_write_history == 0) {
        config_.max_write_history = 1; // 至少保留1条
    }

    page_size_      = 1u << config_.page_size_bits;
    page_size_mask_ = page_size_ - 1;
}

ShadowMemory::~ShadowMemory() {
    // 析构时可选打印统计
    // std::cout << dump_stats() << std::endl;
}

// ============================================================
// 地址转换辅助函数
// ============================================================
ShadowMemory::PageId_t ShadowMemory::addr_to_page_id(Addr_t addr) const {
    return static_cast<PageId_t>(addr >> config_.page_size_bits);
}

uint32_t ShadowMemory::addr_to_page_offset(Addr_t addr) const {
    return static_cast<uint32_t>(addr & page_size_mask_);
}

bool ShadowMemory::validate_addr(Addr_t addr) const {
    if (config_.addr_space_limit > 0 && addr >= config_.addr_space_limit) {
        return false;
    }
    return true;
}

// ============================================================
// Page 管理
// ============================================================
ShadowMemory::Page& ShadowMemory::get_or_create_page(PageId_t page_id) {
    auto it = pages_.find(page_id);
    if (it != pages_.end()) {
        return it->second;
    }

    // 创建新page，预分配所有ByteSlot
    Page new_page;
    new_page.slots.resize(page_size_);  // 默认构造：value=0, initialized=false
    new_page.any_written = false;

    auto [insert_it, _] = pages_.emplace(page_id, std::move(new_page));
    return insert_it->second;
}

const ShadowMemory::Page* ShadowMemory::get_page(PageId_t page_id) const {
    auto it = pages_.find(page_id);
    if (it == pages_.end()) return nullptr;
    return &(it->second);
}

ByteSlot& ShadowMemory::get_or_create_slot(Addr_t addr) {
    PageId_t page_id = addr_to_page_id(addr);
    uint32_t offset  = addr_to_page_offset(addr);
    Page& page = get_or_create_page(page_id);
    return page.slots[offset];
}

const ByteSlot* ShadowMemory::get_slot(Addr_t addr) const {
    PageId_t page_id = addr_to_page_id(addr);
    const Page* page = get_page(page_id);
    if (!page) return nullptr;

    uint32_t offset = addr_to_page_offset(addr);
    const ByteSlot& slot = page->slots[offset];
    // 即使slot存在，如果从未初始化也返回nullptr
    if (!slot.initialized) return nullptr;
    return &slot;
}

// ============================================================
// 写操作：单Byte
// ============================================================
void ShadowMemory::write_byte(Addr_t      addr,
                               uint8_t     value,
                               MstId_t     master_id,
                               TxnId_t     txn_id,
                               SeqNum_t    global_seq,
                               Timestamp_t write_time)
{
    if (!validate_addr(addr)) {
        std::cerr << "[ShadowMemory] WARNING: write to out-of-range address 0x"
                  << std::hex << addr << std::dec << ", ignored.\n";
        return;
    }

    ByteSlot& slot = get_or_create_slot(addr);

    // 更新当前值
    slot.current_value = value;
    slot.initialized   = true;

    // 标记page为已写
    PageId_t page_id = addr_to_page_id(addr);
    pages_[page_id].any_written = true;

    // 构造写记录
    WriteRecord rec;
    rec.value      = value;
    rec.master_id  = master_id;
    rec.txn_id     = txn_id;
    rec.global_seq = global_seq;
    rec.write_time = write_time;

    // 追加到写历史（维护环形缓冲上限）
    slot.write_history.push_back(rec);
    while (slot.write_history.size() > config_.max_write_history) {
        slot.write_history.pop_front();
    }

    total_write_ops_++;
}

// ============================================================
// 写操作：地址范围 + byte enable
// ============================================================
void ShadowMemory::write(Addr_t          base_addr,
                          const Data_t&   data,
                          const ByteEn_t& byte_en,
                          MstId_t         master_id,
                          TxnId_t         txn_id,
                          SeqNum_t        global_seq,
                          Timestamp_t     write_time)
{
    if (data.empty()) return;

    for (size_t i = 0; i < data.size(); ++i) {
        // 检查byte enable：如果byte_en为空则默认全使能
        bool enabled = true;
        if (!byte_en.empty()) {
            if (i < byte_en.size()) {
                enabled = byte_en[i];
            } else {
                enabled = false; // 超出byte_en范围的byte不写
            }
        }

        if (!enabled) continue;

        write_byte(base_addr + i, data[i],
                   master_id, txn_id, global_seq, write_time);
    }

    total_bytes_written_ += data.size();
}

// ============================================================
// 写操作：从Transaction对象直接写入
// ============================================================
void ShadowMemory::write(const Transaction& txn) {
    assert(txn.type == TxnType::WRITE && "ShadowMemory::write() called with non-WRITE txn");
    assert(txn.status == TxnStatus::COMPLETED && "ShadowMemory::write() called with incomplete txn");

    write(txn.addr,
          txn.data,
          txn.byte_enable,
          txn.master_id,
          txn.txn_id,
          txn.global_seq,
          txn.resp_time);
}

// ============================================================
// 读操作：单Byte
// ============================================================
std::optional<uint8_t> ShadowMemory::read_byte(Addr_t addr) const {
    const ByteSlot* slot = get_slot(addr);
    if (!slot) return std::nullopt;
    return slot->current_value;
}

// ============================================================
// 读操作：连续区域
// ============================================================
Data_t ShadowMemory::read(Addr_t base_addr, uint32_t len) const {
    Data_t result(len);

    for (uint32_t i = 0; i < len; ++i) {
        Addr_t addr = base_addr + i;
        const ByteSlot* slot = get_slot(addr);
        if (slot) {
            result[i] = slot->current_value;
        } else {
            result[i] = config_.default_init_value;
        }
    }

    return result;
}

// ============================================================
// 读操作：带日志记录
// ============================================================
Data_t ShadowMemory::read_and_log(Addr_t      base_addr,
                                   uint32_t    len,
                                   MstId_t     reader_master_id,
                                   TxnId_t     reader_txn_id,
                                   Timestamp_t read_time)
{
    Data_t result = read(base_addr, len);

    total_read_ops_++;

    if (config_.track_read_log) {
        for (uint32_t i = 0; i < len; ++i) {
            ReadLogEntry entry;
            entry.addr             = base_addr + i;
            entry.value            = result[i];
            entry.reader_master_id = reader_master_id;
            entry.reader_txn_id    = reader_txn_id;
            entry.read_time        = read_time;
            read_log_.push_back(entry);
        }
    }

    return result;
}

// ============================================================
// 写历史查询：获取完整写历史
// ============================================================
const std::deque<WriteRecord>& ShadowMemory::get_write_history(Addr_t addr) const {
    const ByteSlot* slot = get_slot(addr);
    if (!slot) return empty_history_;
    return slot->write_history;
}

// ============================================================
// 写历史查询：获取最后一次写记录
// ============================================================
std::optional<WriteRecord> ShadowMemory::get_last_write(Addr_t addr) const {
    const ByteSlot* slot = get_slot(addr);
    if (!slot || slot->write_history.empty()) return std::nullopt;
    return slot->write_history.back();
}

// ============================================================
// 写历史查询：时间窗口内所有可能值
// 用于overlap场景：读发生时可能看到窗口内任意一次写的值
//
// 逻辑说明：
//   从写历史中倒序遍历，收集所有 write_time 落在
//   [window_start, window_end] 区间内的写值。
//   同时，也包含窗口之前最后一次写的值（即"旧值"），
//   因为读可能在新写生效之前就完成了。
// ============================================================
std::vector<uint8_t> ShadowMemory::get_possible_values(Addr_t      addr,
                                                        Timestamp_t window_start,
                                                        Timestamp_t window_end) const
{
    std::vector<uint8_t> candidates;
    const ByteSlot* slot = get_slot(addr);

    if (!slot || slot->write_history.empty()) {
        // 从未被写过，唯一可能值是默认初始值
        candidates.push_back(config_.default_init_value);
        return candidates;
    }

    const auto& hist = slot->write_history;
    bool found_pre_window = false;

    // 从最新到最旧遍历
    for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
        if (it->write_time <= window_end && it->write_time >= window_start) {
            // 写落在窗口内 → 读可能看到这个值
            candidates.push_back(it->value);
        } else if (it->write_time < window_start) {
            // 窗口之前的最后一次写 → 读也可能看到这个"旧值"
            if (!found_pre_window) {
                candidates.push_back(it->value);
                found_pre_window = true;
            }
            break; // 更早的写不可能被看到
        }
        // write_time > window_end 的写：发生在读之后，跳过
    }

    // 去重（不同写可能写了相同的值）
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());

    // 如果完全没找到任何候选（不应该发生，但防御性编程）
    if (candidates.empty()) {
        candidates.push_back(config_.default_init_value);
    }

    return candidates;
}

// ============================================================
// 写历史查询：某时刻之前的最新值
// 用途：回溯某个时间点memory应该是什么值
// ============================================================
std::optional<uint8_t> ShadowMemory::get_value_at_time(Addr_t addr,
                                                        Timestamp_t time) const
{
    const ByteSlot* slot = get_slot(addr);
    if (!slot || slot->write_history.empty()) return std::nullopt;

    const auto& hist = slot->write_history;

    // 从最新到最旧遍历，找第一个 write_time <= time 的记录
    for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
        if (it->write_time <= time) {
            return it->value;
        }
    }

    return std::nullopt; // 所有写都发生在time之后
}

// ============================================================
// 状态查询
// ============================================================
bool ShadowMemory::has_been_written(Addr_t addr) const {
    const ByteSlot* slot = get_slot(addr);
    return (slot != nullptr); // get_slot已检查initialized
}

bool ShadowMemory::is_range_written(Addr_t base_addr, uint32_t len) const {
    for (uint32_t i = 0; i < len; ++i) {
        if (!has_been_written(base_addr + i)) {
            return false;
        }
    }
    return true;
}

uint64_t ShadowMemory::get_total_written_bytes() const {
    uint64_t count = 0;
    for (const auto& [page_id, page] : pages_) {
        if (!page.any_written) continue;
        for (const auto& slot : page.slots) {
            if (slot.initialized) count++;
        }
    }
    return count;
}

uint64_t ShadowMemory::get_total_pages_allocated() const {
    return pages_.size();
}

// ============================================================
// 区域操作：预加载数据（模拟firmware预置）
// 不产生写历史记录，仅设置初始值
// ============================================================
void ShadowMemory::preload(Addr_t base_addr, const Data_t& data) {
    for (size_t i = 0; i < data.size(); ++i) {
        Addr_t addr = base_addr + i;

        if (!validate_addr(addr)) {
            std::cerr << "[ShadowMemory] WARNING: preload address 0x"
                      << std::hex << addr << " out of range, skipped.\n";
            continue;
        }

        ByteSlot& slot = get_or_create_slot(addr);
        slot.current_value = data[i];
        slot.initialized   = true;
        // 注意：preload不追加write_history，区别于正常write

        PageId_t page_id = addr_to_page_id(addr);
        pages_[page_id].any_written = true;
    }
}

// ============================================================
// 区域操作：清除某地址范围
// ============================================================
void ShadowMemory::invalidate_range(Addr_t base_addr, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        Addr_t addr = base_addr + i;
        PageId_t page_id = addr_to_page_id(addr);
        uint32_t offset  = addr_to_page_offset(addr);

        auto it = pages_.find(page_id);
        if (it == pages_.end()) continue;

        ByteSlot& slot = it->second.slots[offset];
        slot.current_value = config_.default_init_value;
        slot.initialized   = false;
        slot.write_history.clear();
    }
}

// ============================================================
// 全部清空
// ============================================================
void ShadowMemory::reset() {
    pages_.clear();
    read_log_.clear();
    total_write_ops_     = 0;
    total_read_ops_      = 0;
    total_bytes_written_ = 0;
}

// ============================================================
// 调试：dump某地址范围的当前值（hex格式）
// 输出格式类似hexdump：
//   0x00001000: 01 02 03 04 05 06 07 08  09 0A 0B 0C 0D 0E 0F 10
// ============================================================
std::string ShadowMemory::dump_range(Addr_t base_addr, uint32_t len) const {
    std::ostringstream oss;
    constexpr uint32_t BYTES_PER_LINE = 16;

    oss << "ShadowMemory dump [0x" << std::hex << base_addr
        << " .. 0x" << (base_addr + len - 1) << "]:\n";

    for (uint32_t offset = 0; offset < len; offset += BYTES_PER_LINE) {
        Addr_t line_addr = base_addr + offset;
        oss << "  0x" << std::hex << std::setw(8) << std::setfill('0')
            << line_addr << ": ";

        // Hex部分
        for (uint32_t j = 0; j < BYTES_PER_LINE && (offset + j) < len; ++j) {
            Addr_t addr = base_addr + offset + j;
            auto val = read_byte(addr);
            if (val.has_value()) {
                oss << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(val.value());
            } else {
                oss << "??";  // 未初始化
            }

            if (j == 7) {
                oss << "  ";  // 中间多一个空格分隔
            } else {
                oss << " ";
            }
        }

        // ASCII部分
        oss << " |";
        for (uint32_t j = 0; j < BYTES_PER_LINE && (offset + j) < len; ++j) {
            Addr_t addr = base_addr + offset + j;
            auto val = read_byte(addr);
            if (val.has_value()) {
                char c = static_cast<char>(val.value());
                oss << (std::isprint(c) ? c : '.');
            } else {
                oss << '?';
            }
        }
        oss << "|\n";
    }

    return oss.str();
}

// ============================================================
// 调试：dump某地址的写历史
// ============================================================
std::string ShadowMemory::dump_write_history(Addr_t addr) const {
    std::ostringstream oss;
    oss << "Write history for address 0x" << std::hex << addr << ":\n";

    const auto& hist = get_write_history(addr);
    if (hist.empty()) {
        oss << "  (no writes recorded)\n";
        return oss.str();
    }

    oss << "  Current value: 0x" << std::hex << std::setw(2)
        << std::setfill('0');
    auto cur = read_byte(addr);
    if (cur.has_value()) {
        oss << static_cast<int>(cur.value());
    } else {
        oss << "??";
    }
    oss << "\n";

    oss << "  History (" << std::dec << hist.size() << " entries, oldest first):\n";
    uint32_t idx = 0;
    for (const auto& rec : hist) {
        oss << "    [" << std::setw(3) << idx++ << "] "
            << rec.to_string() << "\n";
    }

    return oss.str();
}

// ============================================================
// 调试：dump统计信息
// ============================================================
std::string ShadowMemory::dump_stats() const {
    std::ostringstream oss;

    oss << "╔══════════════════════════════════════════╗\n"
        << "║       ShadowMemory Statistics            ║\n"
        << "╠══════════════════════════════════════════╣\n"
        << "║  Page size          : " << std::setw(16) << page_size_ << " B ║\n"
        << "║  Pages allocated    : " << std::setw(16) << get_total_pages_allocated() << "   ║\n"
        << "║  Memory footprint   : " << std::setw(16)
        << (get_total_pages_allocated() * page_size_) << " B ║\n"
        << "║  Bytes written      : " << std::setw(16) << get_total_written_bytes() << "   ║\n"
        << "║  Total write ops    : " << std::setw(16) << total_write_ops_ << "   ║\n"
        << "║  Total read ops     : " << std::setw(16) << total_read_ops_ << "   ║\n"
        << "║  Write history depth: " << std::setw(16) << config_.max_write_history << "   ║\n";

    if (config_.track_read_log) {
        oss << "║  Read log entries   : " << std::setw(16) << read_log_.size() << "   ║\n";
    }

    oss << "╚══════════════════════════════════════════╝\n";
    return oss.str();
}

// ============================================================
// 遍历所有已写byte（用于post-sim分析）
// ============================================================
void ShadowMemory::for_each_written_byte(
    const std::function<void(Addr_t, const ByteSlot&)>& visitor) const
{
    if (!visitor) return;

    for (const auto& [page_id, page] : pages_) {
        if (!page.any_written) continue;

        Addr_t page_base = static_cast<Addr_t>(page_id) << config_.page_size_bits;

        for (uint32_t offset = 0; offset < page_size_; ++offset) {
            const ByteSlot& slot = page.slots[offset];
            if (slot.initialized) {
                visitor(page_base + offset, slot);
            }
        }
    }
}