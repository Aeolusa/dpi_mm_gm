import os, sys

path = r'd:\tpu\Work\Prj\dpi_mm_gm\code\cc\src\shadow_memory.cc'
with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

# Replace WriteRecord::to_string
content = content.replace(
'''std::string WriteRecord::to_string() const {
    std::ostringstream oss;
    oss << "[WR SEQ=" << global_seq
        << " MST=" << master_id
        << " TXN=#" << txn_id
        << " val=0x" << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(value)
        << " t=" << std::dec << write_time
        << "]";
    return oss.str();
}''',
'''std::string WriteRecord::to_string() const {
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
}'''
)

# Replace addr_to_page_offset
content = content.replace(
'''uint32_t ShadowMemory::addr_to_page_offset(Addr_t addr) const {
    return static_cast<uint32_t>(addr & page_size_mask_);
}''',
'''uint32_t ShadowMemory::addr_to_page_offset(Addr_t addr) const {
    return static_cast<uint32_t>((addr & page_size_mask_) / BLOCK_SIZE);
}'''
)

# Replace page initialization
content = content.replace('new_page.slots.resize(page_size_);', 'new_page.slots.resize(page_size_ / BLOCK_SIZE);')

# Replace get_or_create_slot
content = content.replace(
'''ByteSlot& ShadowMemory::get_or_create_slot(Addr_t addr) {
    PageId_t page_id = addr_to_page_id(addr);
    uint32_t offset  = addr_to_page_offset(addr);
    Page& page = get_or_create_page(page_id);
    return page.slots[offset];
}''',
'''BlockSlot& ShadowMemory::get_or_create_slot(Addr_t block_addr) {
    PageId_t page_id = addr_to_page_id(block_addr);
    uint32_t offset  = addr_to_page_offset(block_addr);
    Page& page = get_or_create_page(page_id);
    return page.slots[offset];
}'''
)

# Replace get_slot
content = content.replace(
'''const ByteSlot* ShadowMemory::get_slot(Addr_t addr) const {
    PageId_t page_id = addr_to_page_id(addr);
    const Page* page = get_page(page_id);
    if (!page) return nullptr;

    uint32_t offset = addr_to_page_offset(addr);
    const ByteSlot& slot = page->slots[offset];
    // 即使slot存在，如果从未初始化也返回nullptr
    if (!slot.initialized) return nullptr;
    return &slot;
}''',
'''const BlockSlot* ShadowMemory::get_slot(Addr_t block_addr) const {
    PageId_t page_id = addr_to_page_id(block_addr);
    const Page* page = get_page(page_id);
    if (!page) return nullptr;

    uint32_t offset = addr_to_page_offset(block_addr);
    const BlockSlot& slot = page->slots[offset];
    if (slot.init_mask == 0) return nullptr;
    return &slot;
}'''
)

# Replace write_byte with write_block
content = content.replace(
'''void ShadowMemory::write_byte(Addr_t      addr,
                               uint8_t     value,
                               MstId_t     master_id,
                               TxnId_t     txn_id,
                               SeqNum_t    global_seq,
                               Timestamp_t write_time)
{
    if (!validate_addr(addr)) {
        std::cerr << "[ShadowMemory] WARNING: write to out-of-range address 0x"
                  << std::hex << addr << std::dec << ", ignored.\\n";
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
}''',
'''void ShadowMemory::write_block(Addr_t      block_addr,
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
        std::cerr << "[ShadowMemory] WARNING: write to out-of-range address 0x"
                  << std::hex << block_addr << std::dec << ", ignored.\\n";
        return;
    }

    BlockSlot& slot = get_or_create_slot(block_addr);

    // 更新当前值
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
    rec.value      = slot.current_value; // Store the merged value
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
}'''
)

# Replace write
content = content.replace(
'''void ShadowMemory::write(Addr_t          base_addr,
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
}''',
'''void ShadowMemory::write(Addr_t          base_addr,
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
}'''
)

# txn write replacement
content = content.replace('txn.master_id', 'txn.src_id,\n          txn.tgt_id')

# read_byte
content = content.replace(
'''std::optional<uint8_t> ShadowMemory::read_byte(Addr_t addr) const {
    const ByteSlot* slot = get_slot(addr);
    if (!slot) return std::nullopt;
    return slot->current_value;
}''',
'''std::optional<uint8_t> ShadowMemory::read_byte(Addr_t addr) const {
    Addr_t block_addr = addr & ~(static_cast<Addr_t>(BLOCK_SIZE - 1));
    uint32_t offset = addr & (BLOCK_SIZE - 1);
    const BlockSlot* slot = get_slot(block_addr);
    if (!slot || !(slot->init_mask & (1U << offset))) return std::nullopt;
    return slot->current_value[offset];
}'''
)

# read loop inside read
content = content.replace(
'''        const ByteSlot* slot = get_slot(addr);
        if (slot) {
            result[i] = slot->current_value;
        } else {
            result[i] = config_.default_init_value;
        }''',
'''        auto val = read_byte(addr);
        result[i] = val.has_value() ? val.value() : config_.default_init_value;'''
)

# get_write_history
content = content.replace(
'''const std::deque<WriteRecord>& ShadowMemory::get_write_history(Addr_t addr) const {
    const ByteSlot* slot = get_slot(addr);
    if (!slot) return empty_history_;
    return slot->write_history;
}''',
'''const std::deque<WriteRecord>& ShadowMemory::get_write_history(Addr_t addr) const {
    Addr_t block_addr = addr & ~(static_cast<Addr_t>(BLOCK_SIZE - 1));
    const BlockSlot* slot = get_slot(block_addr);
    if (!slot) return empty_history_;
    return slot->write_history;
}'''
)

# get_last_write
content = content.replace(
'''std::optional<WriteRecord> ShadowMemory::get_last_write(Addr_t addr) const {
    const ByteSlot* slot = get_slot(addr);
    if (!slot || slot->write_history.empty()) return std::nullopt;
    return slot->write_history.back();
}''',
'''std::optional<WriteRecord> ShadowMemory::get_last_write(Addr_t addr) const {
    Addr_t block_addr = addr & ~(static_cast<Addr_t>(BLOCK_SIZE - 1));
    const BlockSlot* slot = get_slot(block_addr);
    if (!slot || slot->write_history.empty()) return std::nullopt;
    return slot->write_history.back();
}'''
)

# get_possible_values
content = content.replace(
'''    const ByteSlot* slot = get_slot(addr);

    if (!slot || slot->write_history.empty()) {''',
'''    Addr_t block_addr = addr & ~(static_cast<Addr_t>(BLOCK_SIZE - 1));
    uint32_t offset = addr & (BLOCK_SIZE - 1);
    const BlockSlot* slot = get_slot(block_addr);

    if (!slot || slot->write_history.empty() || !(slot->init_mask & (1U << offset))) {'''
)
content = content.replace('candidates.push_back(it->value);', 'candidates.push_back(it->value[offset]);')

# get_value_at_time
content = content.replace(
'''std::optional<uint8_t> ShadowMemory::get_value_at_time(Addr_t addr,
                                                        Timestamp_t time) const
{
    const ByteSlot* slot = get_slot(addr);
    if (!slot || slot->write_history.empty()) return std::nullopt;''',
'''std::optional<uint8_t> ShadowMemory::get_value_at_time(Addr_t addr,
                                                        Timestamp_t time) const
{
    Addr_t block_addr = addr & ~(static_cast<Addr_t>(BLOCK_SIZE - 1));
    uint32_t offset = addr & (BLOCK_SIZE - 1);
    const BlockSlot* slot = get_slot(block_addr);
    if (!slot || slot->write_history.empty()) return std::nullopt;'''
)
content = content.replace('return it->value;', 'return it->value[offset];')

# has_been_written
content = content.replace(
'''bool ShadowMemory::has_been_written(Addr_t addr) const {
    const ByteSlot* slot = get_slot(addr);
    return (slot != nullptr); // get_slot已检查initialized
}''',
'''bool ShadowMemory::has_been_written(Addr_t addr) const {
    Addr_t block_addr = addr & ~(static_cast<Addr_t>(BLOCK_SIZE - 1));
    uint32_t offset = addr & (BLOCK_SIZE - 1);
    const BlockSlot* slot = get_slot(block_addr);
    return (slot != nullptr && (slot->init_mask & (1U << offset)));
}'''
)

# get_total_written_bytes
content = content.replace(
'''    for (const auto& [page_id, page] : pages_) {
        if (!page.any_written) continue;
        for (const auto& slot : page.slots) {
            if (slot.initialized) count++;
        }
    }''',
'''    for (const auto& [page_id, page] : pages_) {
        if (!page.any_written) continue;
        for (const auto& slot : page.slots) {
            uint32_t mask = slot.init_mask;
            while(mask) {
                count += mask & 1;
                mask >>= 1;
            }
        }
    }'''
)

# preload
content = content.replace('ByteSlot& slot = get_or_create_slot(addr);', 'BlockSlot& slot = get_or_create_slot(addr & ~(static_cast<Addr_t>(BLOCK_SIZE - 1)));')
content = content.replace('slot.current_value = data[i];', 'slot.current_value[addr & (BLOCK_SIZE - 1)] = data[i];')
content = content.replace('slot.initialized   = true;', 'slot.init_mask |= (1U << (addr & (BLOCK_SIZE - 1)));')

# invalidate_range
content = content.replace(
'''        ByteSlot& slot = it->second.slots[offset];
        slot.current_value = config_.default_init_value;
        slot.initialized   = false;
        slot.write_history.clear();''',
'''        // Needs to properly zero out bytes...
        // For simplicity, just clearing whole block if we hit it
        BlockSlot& slot = it->second.slots[offset];
        slot.current_value.fill(config_.default_init_value);
        slot.init_mask = 0;
        slot.write_history.clear();'''
)

# for_each_written_byte
content = content.replace('for_each_written_byte', 'for_each_written_block')
content = content.replace('const std::function<void(Addr_t, const ByteSlot&)>& visitor', 'const std::function<void(Addr_t, const BlockSlot&)>& visitor')
content = content.replace('const ByteSlot& slot = page.slots[offset];', 'const BlockSlot& slot = page.slots[offset];')
content = content.replace('if (slot.initialized) {', 'if (slot.init_mask != 0) {')
content = content.replace('visitor(page_base + offset, slot);', 'visitor(page_base + offset * BLOCK_SIZE, slot);')

with open(path, 'w', encoding='utf-8') as f:
    f.write(content)

print('Success')
