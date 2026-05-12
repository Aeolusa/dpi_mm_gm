// ============================================================
// file: src/shadow_memory.cpp
// ============================================================

#include "shadow_memory.h"
#include "logger.h"

#include <cassert>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>
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
        << " SRC=" << src_id << " TGT=" << tgt_id
        << " TXN=#" << txn_id
        << " val=";
    for(int i=0; i<BLOCK_SIZE; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)value[i];
    }
    oss << " t=" << std::dec << write_time
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
    
    if (page_size_ < BLOCK_SIZE) {
        throw std::invalid_argument("ShadowMemory: page_size must be >= BLOCK_SIZE");
    }
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
    return static_cast<uint32_t>((addr & page_size_mask_) / BLOCK_SIZE);
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

    // 创建新page，预分配所有BlockSlot
    Page new_page;
    new_page.slots.resize(page_size_ / BLOCK_SIZE);
    new_page.any_written = false;

    auto [insert_it, _] = pages_.emplace(page_id, std::move(new_page));
    return insert_it->second;
}

const ShadowMemory::Page* ShadowMemory::get_page(PageId_t page_id) const {
    auto it = pages_.find(page_id);
    if (it == pages_.end()) return nullptr;
    return &(it->second);
}

BlockSlot& ShadowMemory::get_or_create_slot(Addr_t block_addr) {
    PageId_t page_id = addr_to_page_id(block_addr);
    uint32_t offset  = addr_to_page_offset(block_addr);
    Page& page = get_or_create_page(page_id);
    return page.slots[offset];
}

const BlockSlot* ShadowMemory::get_slot(Addr_t block_addr) const {
    PageId_t page_id = addr_to_page_id(block_addr);
    const Page* page = get_page(page_id);
    if (!page) return nullptr;

    uint32_t offset = addr_to_page_offset(block_addr);
    const BlockSlot& slot = page->slots[offset];
    if (slot.init_mask == 0) return nullptr;
    return &slot;
}

// ============================================================
// 写操作：Block写入
// ============================================================
void ShadowMemory::write_block(Addr_t      block_addr,
                               const std::array<uint8_t, BLOCK_SIZE>& value,
                               uint32_t    byte_en_mask,
                               uint32_t    src_id,
                               uint32_t    tgt_id,
                               TxnId_t     txn_id,
                               SeqNum_t    global_seq,
                               Timestamp_t write_time)
{
    if (byte_en_mask == 0) return;

    if (!validate_addr(block_addr)) {
        LOG_DEBUG("[ShadowMemory] WARNING: write to out-of-range address 0x"
                  << std::hex << block_addr << std::dec << ", ignored.\n");
        return;
    }

    BlockSlot& slot = get_or_create_slot(block_addr);

    // Apply byte_en_mask and merge
    for (uint32_t i = 0; i < BLOCK_SIZE; ++i) {
        if (byte_en_mask & (1U << i)) {
            slot.current_value[i] = value[i];
            slot.init_mask |= (1U << i);
        }
    }

    // 标记page为已写
    PageId_t page_id = addr_to_page_id(block_addr);
    pages_[page_id].any_written = true;

    // 构造写记录
    WriteRecord rec;
    rec.value      = slot.current_value; // Store the merged value!
    rec.src_id     = src_id;
    rec.tgt_id     = tgt_id;
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
                          uint32_t        src_id,
                          uint32_t        tgt_id,
                          TxnId_t         txn_id,
                          SeqNum_t        global_seq,
                          Timestamp_t     write_time)
{
    if (data.empty()) return;

    Addr_t current_addr = base_addr;
    uint32_t data_offset = 0;
    
    while (data_offset < data.size()) {
        Addr_t block_addr = current_addr & ~(static_cast<Addr_t>(BLOCK_SIZE - 1));
        uint32_t block_offset = current_addr & (BLOCK_SIZE - 1);
        
        std::array<uint8_t, BLOCK_SIZE> block_val = {0};
        uint32_t block_en_mask = 0;
        
        while (data_offset < data.size() && block_offset < BLOCK_SIZE) {
            bool enabled = true;
            if (!byte_en.empty()) {
                enabled = (data_offset < byte_en.size()) ? byte_en[data_offset] : false;
            }
            if (enabled) {
                block_val[block_offset] = data[data_offset];
                block_en_mask |= (1U << block_offset);
            }
            data_offset++;
            current_addr++;
            block_offset++;
        }
        
        write_block(block_addr, block_val, block_en_mask, src_id, tgt_id, txn_id, global_seq, write_time);
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
          txn.src_id,
          txn.tgt_id,
          txn.txn_id,
          txn.global_seq,
          txn.resp_time);
}

// ============================================================
// 读操作：单Byte
// ============================================================
std::optional<uint8_t> ShadowMemory::read_byte(Addr_t addr) const {
    Addr_t block_addr = addr & ~(static_cast<Addr_t>(BLOCK_SIZE - 1));
    uint32_t offset = addr & (BLOCK_SIZE - 1);
    const BlockSlot* slot = get_slot(block_addr);
    if (!slot || !(slot->init_mask & (1U << offset))) return std::nullopt;
    return slot->current_value[offset];
}

// ============================================================
// 读操作：连续区域
// ============================================================
Data_t ShadowMemory::read(Addr_t base_addr, uint32_t len) const {
    Data_t result(len);

    for (uint32_t i = 0; i < len; ++i) {
        Addr_t addr = base_addr + i;
        auto val = read_byte(addr);
        result[i] = val.has_value() ? val.value() : config_.default_init_value;
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
    Addr_t block_addr = addr & ~(static_cast<Addr_t>(BLOCK_SIZE - 1));
    const BlockSlot* slot = get_slot(block_addr);
    if (!slot) return empty_history_;
    return slot->write_history;
}

// ============================================================
// 写历史查询：获取最后一次写记录
// ============================================================
std::optional<WriteRecord> ShadowMemory::get_last_write(Addr_t addr) const {
    Addr_t block_addr = addr & ~(static_cast<Addr_t>(BLOCK_SIZE - 1));
    const BlockSlot* slot = get_slot(block_addr);
    if (!slot || slot->write_history.empty()) return std::nullopt;
    return slot->write_history.back();
}

// ============================================================
// 写历史查询：时间窗口内所有可能值
// ============================================================
std::vector<uint8_t> ShadowMemory::get_possible_values(Addr_t      addr,
                                                        Timestamp_t window_start,
                                                        Timestamp_t window_end) const
{
    std::vector<uint8_t> candidates;
    Addr_t block_addr = addr & ~(static_cast<Addr_t>(BLOCK_SIZE - 1));
    uint32_t offset = addr & (BLOCK_SIZE - 1);
    const BlockSlot* slot = get_slot(block_addr);

    if (!slot || slot->write_history.empty() || !(slot->init_mask & (1U << offset))) {
        candidates.push_back(config_.default_init_value);
        return candidates;
    }

    const auto& hist = slot->write_history;
    bool found_pre_window = false;

    // 从最新到最旧遍历
    for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
        if (it->write_time <= window_end && it->write_time >= window_start) {
            candidates.push_back(it->value[offset]);
        } else if (it->write_time < window_start) {
            if (!found_pre_window) {
                candidates.push_back(it->value[offset]);
                found_pre_window = true;
            }
            break; 
        }
    }

    // 去重
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());

    if (candidates.empty()) {
        candidates.push_back(config_.default_init_value);
    }

    return candidates;
}

// ============================================================
// 写历史查询：某时刻之前的最新值
// ============================================================
std::optional<uint8_t> ShadowMemory::get_value_at_time(Addr_t addr,
                                                        Timestamp_t time) const
{
    Addr_t block_addr = addr & ~(static_cast<Addr_t>(BLOCK_SIZE - 1));
    uint32_t offset = addr & (BLOCK_SIZE - 1);
    const BlockSlot* slot = get_slot(block_addr);
    if (!slot || slot->write_history.empty()) return std::nullopt;

    const auto& hist = slot->write_history;

    for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
        if (it->write_time <= time) {
            return it->value[offset];
        }
    }

    return std::nullopt; 
}

// ============================================================
// 状态查询
// ============================================================
bool ShadowMemory::has_been_written(Addr_t addr) const {
    Addr_t block_addr = addr & ~(static_cast<Addr_t>(BLOCK_SIZE - 1));
    uint32_t offset = addr & (BLOCK_SIZE - 1);
    const BlockSlot* slot = get_slot(block_addr);
    return (slot != nullptr && (slot->init_mask & (1U << offset)));
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
            uint32_t mask = slot.init_mask;
            while(mask) {
                count += mask & 1;
                mask >>= 1;
            }
        }
    }
    return count;
}

uint64_t ShadowMemory::get_total_pages_allocated() const {
    return pages_.size();
}

// ============================================================
// 区域操作：预加载数据
// ============================================================
void ShadowMemory::preload(Addr_t base_addr, const Data_t& data) {
    for (size_t i = 0; i < data.size(); ++i) {
        Addr_t addr = base_addr + i;

        if (!validate_addr(addr)) {
            LOG_DEBUG("[ShadowMemory] WARNING: preload address 0x"
                      << std::hex << addr << " out of range, skipped.\n");
            continue;
        }

        BlockSlot& slot = get_or_create_slot(addr & ~(static_cast<Addr_t>(BLOCK_SIZE - 1)));
        slot.current_value[addr & (BLOCK_SIZE - 1)] = data[i];
        slot.init_mask |= (1U << (addr & (BLOCK_SIZE - 1)));

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

        BlockSlot& slot = it->second.slots[offset];
        slot.current_value[addr & (BLOCK_SIZE - 1)] = config_.default_init_value;
        slot.init_mask &= ~(1U << (addr & (BLOCK_SIZE - 1)));
        // Note: write_history cannot be partially cleared easily per byte. 
        // We will just clear the whole history for the block if this is called.
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
// 遍历所有已写Block
// ============================================================
void ShadowMemory::for_each_written_block(
    const std::function<void(Addr_t, const BlockSlot&)>& visitor) const
{
    if (!visitor) return;

    for (const auto& [page_id, page] : pages_) {
        if (!page.any_written) continue;

        Addr_t page_base = static_cast<Addr_t>(page_id) << config_.page_size_bits;

        for (uint32_t offset = 0; offset < page.slots.size(); ++offset) {
            const BlockSlot& slot = page.slots[offset];
            if (slot.init_mask != 0) {
                visitor(page_base + offset * BLOCK_SIZE, slot);
            }
        }
    }
}