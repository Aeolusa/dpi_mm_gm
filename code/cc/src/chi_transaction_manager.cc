// ============================================================
// file: src/chi_transaction_manager.cc
// CHI 事务管理器实现
//   - SM 写事务: expected_flits = secvec popcount
//   - 其他事务:  expected_flits = size_to_flits(size)
//   - 多通道并发保护 (mutex)
// ============================================================
#include "chi_transaction_manager.h"
#include "logger.h"
#include <cstring>
#include <cassert>

#ifndef _MSC_VER
extern "C" __attribute__((weak)) void sv_set_gm_error() {}
extern "C" __attribute__((weak)) void sv_clr_gm_error() {}
#else
extern "C" void sv_set_gm_error() {}
extern "C" void sv_clr_gm_error() {}
#endif

// ============================================================
// 构造
// ============================================================
ChiTransactionManager::ChiTransactionManager(bool strict_mode,
                                               uint64_t addr_filter_mask)
    : checker_(strict_mode), global_seq_(1), addr_filter_mask_(addr_filter_mask)
{}

// ============================================================
// Soft reset
// ============================================================
void ChiTransactionManager::check_soft_ctrl(bool soft_ctrl) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (soft_ctrl && !last_soft_ctrl_) {
        LOG_INFO("[CHI MGR] Using soft ctrl & GM reset\n");
        outstanding_reads_.clear();
        outstanding_writes_.clear();
        outstanding_dataless_.clear();
        outstanding_snoops_.clear();
        dbid_to_txnid_.clear();
        global_seq_ = 1;
        has_error_ = false;
        checker_.reset();
    }
    last_soft_ctrl_ = soft_ctrl;
}

void ChiTransactionManager::set_error() {
    has_error_ = true;
    sv_set_gm_error();
}

void ChiTransactionManager::clr_error() {
    has_error_ = false;
    sv_clr_gm_error();
}

int ChiTransactionManager::get_error() const {
    return has_error_ ? 1 : 0;
}

// ============================================================
// 内部辅助：将 128-byte 缓冲区组装为 Transaction 并送入 Checker
// ============================================================
void ChiTransactionManager::finalize_write(ChiOutstandingWrite& req,
                                           Timestamp_t resp_time)
{
    // secvec 决定哪些 32B 段有效，以此构造 byte_enable
    // 数据缓冲已按 dataid 散落填入，此处直接转换为 Data_t/ByteEn_t
    Data_t   data   (CHI_CL_BYTES, 0);
    ByteEn_t byte_en(CHI_CL_BYTES, false);

    for (uint32_t i = 0; i < CHI_CL_BYTES; ++i) {
        data[i]    = req.data_buf[i];
        byte_en[i] = req.be_buf[i];
    }

    Transaction txn;
    txn.txn_id      = req.key.txn_id;   // 使用原始 CHI txnid
    txn.global_seq  = global_seq_++;
    txn.src_id      = req.key.mst_idx;  // src_id 复用为 mst_idx
    txn.tgt_id      = 0;
    txn.dbid        = req.dbid;
    txn.type        = TxnType::WRITE;
    txn.addr        = align_to_cacheline(req.addr);  // cacheline 对齐
    txn.orig_addr   = req.addr;
    txn.secvec      = req.secvec;
    txn.size        = CHI_CL_BYTES;   // 固定128字节缓冲
    txn.burst_len   = 1;
    txn.data        = std::move(data);
    txn.byte_enable = std::move(byte_en);
    txn.req_time    = req.req_time;
    txn.resp_time   = resp_time;
    txn.status      = TxnStatus::COMPLETED;

    std::ostringstream data_oss;
    std::ostringstream be_oss;
    for (int32_t i = CHI_CL_BYTES - 1; i >= 0; --i) {
        data_oss << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<uint32_t>(txn.data[i]);
        if (i > 0 && i % 32 == 0) data_oss << "_";
    }
    for (int32_t i = (CHI_CL_BYTES / 4) - 1; i >= 0; --i) {
        int val = 0;
        if (txn.byte_enable[i * 4 + 3]) val |= 8;
        if (txn.byte_enable[i * 4 + 2]) val |= 4;
        if (txn.byte_enable[i * 4 + 1]) val |= 2;
        if (txn.byte_enable[i * 4 + 0]) val |= 1;
        be_oss << std::hex << val;
        if (i > 0 && (i * 4) % 32 == 0) be_oss << "_";
    }

    LOG_INFO(
        "[CHI MGR] Completed Write: mst=" << get_mst_name(req.key.mst_idx)
        << " txnid=" << std::hex << req.key.txn_id
        << " addr=0x" << std::hex << txn.addr
        << " size="  << std::dec << txn.size
        << "\n  DATA:" << data_oss.str()
        << "\n  BE:" << be_oss.str() << "\n"
    );

    checker_.process_write(txn);
}

void ChiTransactionManager::finalize_read(ChiOutstandingRead& req,
                                          Timestamp_t resp_time)
{
    Data_t   data   (CHI_CL_BYTES, 0);
    ByteEn_t byte_en(CHI_CL_BYTES, false);

    for (uint32_t i = 0; i < CHI_CL_BYTES; ++i) {
        data[i]    = req.data_buf[i];
        byte_en[i] = req.be_buf[i];
    }

    Transaction txn;
    txn.txn_id      = req.key.txn_id;
    txn.global_seq  = global_seq_++;
    txn.src_id      = req.key.mst_idx;
    txn.tgt_id      = 0;
    txn.dbid        = 0;
    txn.type        = TxnType::READ;
    txn.addr        = align_to_cacheline(req.addr);
    txn.orig_addr   = req.addr;
    txn.secvec      = req.secvec;
    txn.size        = CHI_CL_BYTES;
    txn.burst_len   = 1;
    txn.data        = std::move(data);
    txn.byte_enable = std::move(byte_en);
    txn.req_time    = req.req_time;
    txn.resp_time   = resp_time;
    txn.status      = TxnStatus::COMPLETED;

    std::ostringstream data_oss;
    std::ostringstream be_oss;
    for (int32_t i = CHI_CL_BYTES - 1; i >= 0; --i) {
        data_oss << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<uint32_t>(txn.data[i]);
        if (i > 0 && i % 32 == 0) data_oss << "_";
    }
    for (int32_t i = (CHI_CL_BYTES / 4) - 1; i >= 0; --i) {
        int val = 0;
        if (txn.byte_enable[i * 4 + 3]) val |= 8;
        if (txn.byte_enable[i * 4 + 2]) val |= 4;
        if (txn.byte_enable[i * 4 + 1]) val |= 2;
        if (txn.byte_enable[i * 4 + 0]) val |= 1;
        be_oss << std::hex << val;
        if (i > 0 && (i * 4) % 32 == 0) be_oss << "_";
    }

    LOG_INFO(
        "[CHI MGR] Completed Read: mst=" << get_mst_name(req.key.mst_idx)
        << " txnid=" << std::hex << req.key.txn_id
        << " addr=0x" << std::hex << txn.addr
        << " size="  << std::dec << txn.size
        << "\n  DATA:" << data_oss.str()
        << "\n  BE:" << be_oss.str() << "\n"
    );

    CheckReport report = checker_.process_read(txn, req.filtered);
    if (report.result != CheckResult::PASS &&
        report.result != CheckResult::PASS_WITH_RELAXED_ORDER) {
        LOG_ERROR("\n" << report.message << "\n");
        set_error(); 
    }
}

void ChiTransactionManager::finalize_snoop(ChiOutstandingSnoop& req,
                                           Timestamp_t resp_time)
{
    Data_t      data    (CHI_CL_BYTES, 0);
    ByteEn_t    byte_en (CHI_CL_BYTES, false);

    for (uint32_t i = 0; i < CHI_CL_BYTES; ++i) {
        data[i]     = req.data_buf[i];
        byte_en[i]  = req.be_buf[i];
    }

    Transaction txn;
    txn.txn_id      = req.key.txn_id;
    txn.global_seq  = global_seq_++;
    txn.src_id      = req.key.mst_idx;
    txn.tgt_id      = 0;
    txn.dbid        = 0;
    txn.type        = TxnType::WRITE;
    txn.addr        = align_to_cacheline(req.addr);
    txn.orig_addr   = req.addr;
    txn.size        = CHI_CL_BYTES;
    txn.burst_len   = 1;
    txn.data        = std::move(data);
    txn.byte_enable = std::move(byte_en);
    txn.req_time    = req.req_time;
    txn.resp_time   = resp_time;
    txn.status      = TxnStatus::COMPLETED;

    std::ostringstream data_oss;
    std::ostringstream be_oss;
    for (int32_t i = CHI_CL_BYTES - 1; i >= 0; --i) {
        data_oss << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<uint32_t>(txn.data[i]);
        if (i > 0 && i % 32 == 0) data_oss << "_";
    }
    for (int32_t i = (CHI_CL_BYTES / 4) - 1; i >= 0; --i) {
        int val = 0;
        if (txn.byte_enable[i * 4 + 3]) val |= 8;
        if (txn.byte_enable[i * 4 + 2]) val |= 4;
        if (txn.byte_enable[i * 4 + 1]) val |= 2;
        if (txn.byte_enable[i * 4 + 0]) val |= 1;
        be_oss << std::hex << val;
        if (i > 0 && (i * 4) % 32 == 0) be_oss << "_";
    }

    LOG_INFO(
        "[CHI MGR] Completed Snoop write: mst=" << get_mst_name(req.key.mst_idx)
        << " txnid=" << std::hex << req.key.txn_id
        << " addr=0x" << std::hex << txn.addr
        << "\n  DATA:" << data_oss.str()
        << "\n  BE:" << be_oss.str() << "\n"
    );

    checker_.process_write(txn);
}

// ============================================================
// process_txreq: 由 txreq_flitv 触发
//   opcode 区分 RdNoSnp / WrNoSnp
//   expected_flits 计算规则：
//     - SM 写请求: secvec popcount
//     - 其他请求:  size_to_flits(size)
//   secvec 编码与 dataid 的对应关系:
//     secvec[0]=1 → dataid=0,  secvec[1]=1 → dataid=2,
//     secvec[2]=1 → dataid=4,  secvec[3]=1 → dataid=6
// ============================================================
void ChiTransactionManager::process_txreq(uint32_t mst_idx, uint32_t txn_id,
                                           Addr_t addr, uint32_t size,
                                           uint32_t opcode, uint32_t secvec,
                                           Timestamp_t req_time, bool is_sm)
{
    std::lock_guard<std::mutex> lock(mtx_);

    MstKey key = {mst_idx, txn_id};

    if (chi_is_reqlcrdreturn(opcode)) return;

    if (chi_is_read_opcode(opcode)) {
        // ---- 读请求：所有类型均使用 size 决定 flit 数 ----
        if (outstanding_reads_.count(key)) {
            LOG_DEBUG("[CHI MGR] WARN: duplicate txreq read "
                      << "mst=" << get_mst_name(mst_idx) << " txnid=" << txn_id << "\n");
        }
        uint32_t exp_flits = size_to_flits(size);

        ChiOutstandingRead req{};
        req.key             = key;
        req.addr            = addr;
        req.opcode          = opcode;
        req.secvec          = secvec;
        req.orig_size       = size;
        req.expected_flits  = exp_flits;
        req.req_time        = req_time;
        req.filtered        = is_addr_filtered(addr);
        outstanding_reads_[key] = req;

    } else if (chi_is_write_opcode(opcode)) {
        // ---- 写请求 ----
        if (outstanding_writes_.count(key)) {
            LOG_DEBUG("[CHI MGR] WARN: duplicate txreq write "
                      << "mst=" << get_mst_name(mst_idx) << " txnid=" << txn_id << "\n");
        }

        // SM 写使用 secvec popcount；非 SM 写使用 size 编码
        uint32_t exp_flits = is_sm ? secvec_to_flits(secvec)
                                   : size_to_flits(size);
        if (exp_flits == 0) exp_flits = 1; // 至少1笔（防御）

        ChiOutstandingWrite req{};
        req.key             = key;
        req.addr            = addr;
        req.opcode          = opcode;
        req.secvec          = secvec;
        req.orig_size       = size;
        req.is_sm           = is_sm;
        req.expected_flits  = exp_flits;
        req.req_time        = req_time;
        req.filtered        = is_addr_filtered(addr);
        outstanding_writes_[key] = req;
    
    } else if (chi_is_dataless_opcode(opcode)) {
        if (outstanding_dataless_.count(key)) {
            LOG_DEBUG("[CHI MGR] WARN: duplicate txreq dataless "
                      << "mst=" << get_mst_name(mst_idx) << " txnid=" << txn_id << "\n");
        }
        ChiOutstandingDataless req{};
        req.key             = key;
        req.addr            = addr;
        req.opcode          = opcode;
        req.req_time        = req_time;
        outstanding_dataless_[key] = req;

    } else {
        LOG_DEBUG("[CHI MGR] WARN: unknown txreq opcode=0x"
                  << std::hex << opcode << std::dec
                  << " mst=" << get_mst_name(mst_idx) << " txnid=" << txn_id 
                  << " addr=0x" << std::hex << addr << std::dec
                  << " size=" << size << " secvec=0x" << std::hex << secvec << std::dec
                  << " time=" << req_time << " is_sm=" << is_sm << "\n");
        set_error();
    }
}

// ============================================================
// process_rxrsp_dbid: 由 rxrsp_flitv 触发
//   rxrsp.txnid 匹配 txreq.txnid（同一 mst 内）
//   存储 dbid，建立 (mst_idx, dbid) → txnid 逆向映射
// ============================================================
void ChiTransactionManager::process_rxrsp_dbid(uint32_t mst_idx,
                                                uint32_t txn_id,
                                                uint32_t opcode,
                                                uint32_t dbid)
{
    std::lock_guard<std::mutex> lock(mtx_);

    if (chi_is_rsplcrdreturn(opcode)) return;

    if (chi_is_comp_rsp_opcode(opcode)) {
        MstKey key = {mst_idx, txn_id};
        auto it = outstanding_dataless_.find(key);
        if (it != outstanding_dataless_.end()) {
            LOG_INFO("[CHI MGR] Completed DATALESS: mst=" << get_mst_name(mst_idx)
                     << " txnid=" << std::hex << txn_id << std::dec << opcode << std::dec
                     << " addr=0x" << std::hex << it->second.addr << std::dec << "\n");
            outstanding_dataless_.erase(it);
        } else {
            // Try to find (DBIDResp + Comp) in in-fly write transaction
            auto wit = outstanding_writes_.find(key);
            if (wit != outstanding_writes_.end()) {
                wit->second.comp_received = true;
                LOG_INFO("[CHI MGR] Write Comp received: mst=" << get_mst_name(mst_idx)
                         << " txnid=" << std::hex << txn_id 
                         << " data_complete=" << wit->second.data_complete << "\n");
                try_finalize_write(wit);
            } else {
                LOG_ERROR("[CHI MGR] ERROR: rxrsp Comp cannot find dataless/write req" 
                         << " mst=" << get_mst_name(mst_idx)
                         << " txnid=" << std::hex << txn_id << "\n");
                set_error();
            }
        }
        return;
    }

    if (opcode != 0 && !chi_is_dbid_rsp_opcode(opcode)) {
        LOG_ERROR("[CHI MGR] ERROR: unknown rxrsp opcode=0x " 
                    << std::hex << opcode << std::dec
                    << " mst=" << get_mst_name(mst_idx) << " txnid=" << txn_id << " dbid=" << dbid << "\n");
        set_error();
    }

    MstKey key = {mst_idx, txn_id};
    auto it = outstanding_writes_.find(key);
    if (it == outstanding_writes_.end()) {
        LOG_ERROR("[CHI MGR] ERROR: rxrsp cannot find write "
                  << "mst=" << get_mst_name(mst_idx) << " txnid=" << std::hex << txn_id << "\n");
        dump_outstanding_writes(mst_idx);
        set_error();
        return;
    }

    it->second.dbid       = dbid;
    it->second.dbid_valid = true;

    if (opcode == static_cast<int>(ChiRspOpocde::CompDBIDResp) ||
        opcode == static_cast<int>(ChiRspOpocde::Host_CompDBIDResp)) {
        it->second.comp_received = true;
    }

    // 建立逆向映射: {mst_idx, dbid} → txnid
    // txdat 到来时使用 txdat.txnid = dbid
    MstKey dbid_key = {mst_idx, dbid};
    dbid_to_txnid_[dbid_key].push_back(txn_id);
}

// ============================================================
// process_txdat: 由 txdat_flitv 触发 (NCBWrData)
//   注意：txdat.txnid 字段实际携带的是 dbid！
//         即 NCBWrData.txnid = DBIDResp.dbid
//   dataid 编码: 0/2/4/6 → 对应 cacheline 中的 32B 段偏移
// ============================================================
void ChiTransactionManager::process_txdat(uint32_t mst_idx,
                                           uint32_t txn_id_as_dbid,
                                           uint32_t opcode,
                                           uint32_t dataid,
                                           const uint8_t* flit_data,
                                           uint32_t be_mask,
                                           uint32_t data_cnt)
{
    // SNP process
    if (chi_is_snp_resp_data_opcode(opcode)) {
        std::lock_guard<std::mutex> lock(mtx_);

        MstKey key = {mst_idx, txn_id_as_dbid};
        auto it = outstanding_snoops_.find(key);
        if (it == outstanding_snoops_.end()) {
            LOG_ERROR("[CHI MGR] ERROR: txdat SnpRespData cannot find Snoop "
                      << " mst=" << get_mst_name(mst_idx) << " txnid=" << std::hex 
                      << txn_id_as_dbid << " dataid=" << dataid << "\n");
            set_error();
            return;
        }

        ChiOutstandingSnoop& req = it->second;

        uint32_t chunk_idx = dataid / 2;
        if ((req.received_dataids_mask & (1 << chunk_idx)) != 0) {
            return; // same flit, silent skip
        }
        req.received_dataids_mask |= (1 << chunk_idx);
        uint32_t byte_offset = dataid_to_offset(dataid);
        uint32_t mask = be_mask;
        for (uint32_t i = 0; i < 32; ++i) {
            uint32_t idx = byte_offset + i;
            req.data_buf[idx] = flit_data[i];
            req.be_buf[idx] = (mask & 0x1);
            mask >>= 1;
        }
        req.accumulated_flits++;

        if (req.accumulated_flits >= req.expected_flits) {
            finalize_snoop(req, 0);
            outstanding_snoops_.erase(it);
        }
        return;
    }

    if (!chi_is_write_data_opcode(opcode)) {
        return;
    }

    std::lock_guard<std::mutex> lock(mtx_);

    // 用 (mst_idx, dbid) 找回原始 txnid
    MstKey dbid_key = {mst_idx, txn_id_as_dbid};
    auto dbid_it = dbid_to_txnid_.find(dbid_key);
    if (dbid_it == dbid_to_txnid_.end() || dbid_it->second.empty()) {
        LOG_ERROR("[CHI MGR] ERROR: txdat cannot find dbid mapping "
                  << "mst=" << get_mst_name(mst_idx) << " dbid=" << txn_id_as_dbid << "\n");
        dump_outstanding_writes(mst_idx);
        set_error();
        return;
    }

    uint32_t orig_txnid = dbid_it->second.front();
    MstKey orig_key = {mst_idx, orig_txnid};
    auto req_it = outstanding_writes_.find(orig_key);
    if (req_it == outstanding_writes_.end()) {
        LOG_ERROR("[CHI MGR] ERROR: txdat found dbid map but no write "
                  << "mst=" << get_mst_name(mst_idx) << " txnid=" << orig_txnid << "\n");
        dump_outstanding_writes(mst_idx);
        set_error();
        return;
    }

    ChiOutstandingWrite& req = req_it->second;

    // SM 写事务：收到首笔 txdat 时，用 data_cnt 覆盖 expected_flits
    /*
        data_cnt encode:
        data_cnt = 0 -> 1 data_flits
        data_cnt = 1 -> 2 data flits
        data_cnt = 2 -> 3 data_flits
        data_cnt = 3 -> 4 data flits
    */
    if (req.is_sm && !req.data_cnt_applied) {
        uint32_t pop = secvec_to_flits(req.secvec);
        if (data_cnt >= pop) {
            req.expected_flits = data_cnt;
        } else {
            req.expected_flits = data_cnt + 1;
        }
        req.data_cnt_applied = true;
    }

    // 按 dataid 确定数据在 128B 缓冲中的偏移
    uint32_t byte_offset = dataid_to_offset(dataid);
    if (byte_offset + CHI_FLIT_BYTES > CHI_CL_BYTES) {
        LOG_ERROR("[CHI MGR] ERROR: txdat dataid=" << dataid
                  << " yields out-of-range offset=" << byte_offset << "\n");
        set_error();
        return;
    }

    // 填入数据和 byte enable
    uint32_t mask = be_mask;
    for (uint32_t i = 0; i < 32; ++i) {
        uint32_t idx = byte_offset + i;
        req.data_buf[idx] = flit_data[i];
        req.be_buf  [idx] = mask & 0x1;
        mask >>= 1;
    }
    req.accumulated_flits++;

    // 检查是否收齐所有 flit
    if (req.accumulated_flits >= req.expected_flits) {
        req.data_complete = true;
        
        // When all data is collected, the dbid mapping is released
        // In case latter trans using the same dbid, and the head of FIFO is still the old one
        dbid_it->second.pop_front();
        if (dbid_it->second.empty()) {
            dbid_to_txnid_.erase(dbid_it);
        }
        
        try_finalize_write(req_it);
    }
}

void ChiTransactionManager::try_finalize_write(
    std::unordered_map<MstKey, ChiOutstandingWrite>::iterator it
) {
    ChiOutstandingWrite& req = it->second;

    if (!req.data_complete || !req.comp_received) {
        return;
    }

    Timestamp_t resp_time = 0;
    finalize_write(req, resp_time);

    outstanding_writes_.erase(it);
}

// ============================================================
// process_rxdat: 由 rxdat_flitv 触发 (CompData)
//   rxdat.txnid = txreq.txnid（读事务）
//   dataid 编码: 0/2/4/6 → 对应 cacheline 中的 32B 段偏移
// ============================================================
void ChiTransactionManager::process_rxdat(uint32_t mst_idx,
                                           uint32_t txn_id,
                                           uint32_t opcode,
                                           uint32_t dataid,
                                           const uint8_t* flit_data,
                                           uint32_t be_mask,
                                           uint32_t data_cnt)
{
    std::lock_guard<std::mutex> lock(mtx_);

    MstKey key = {mst_idx, txn_id};
    auto it = outstanding_reads_.find(key);
    if (it == outstanding_reads_.end()) {
        LOG_ERROR("[CHI MGR] ERROR: rxdat cannot find read "
                  << "mst=" << get_mst_name(mst_idx) << " txnid=" << txn_id << "\n");
        set_error();
        return;
    }

    ChiOutstandingRead& req = it->second;

    // 按 dataid 放置数据
    uint32_t byte_offset = dataid_to_offset(dataid);
    if (byte_offset + CHI_FLIT_BYTES > CHI_CL_BYTES) {
        LOG_ERROR("[CHI MGR] ERROR: rxdat dataid=" << dataid
                  << " out-of-range offset=" << byte_offset << "\n");
        set_error();
        return;
    }

    uint32_t mask = be_mask;
    for (uint32_t i = 0; i < 32; ++i) {
        uint32_t idx = byte_offset + i;
        req.data_buf[idx] = flit_data[i];
        req.be_buf  [idx] = mask & 0x1;
        mask >>= 1;
    }
    req.accumulated_flits++;

    if (req.accumulated_flits >= req.expected_flits) {
        // 使用当前仿真时间（暂用0，txdat也一样；可从DPI侧传入）
        finalize_read(req, 0);
        outstanding_reads_.erase(it);
    }
}

// ============================================================
// process_snoop
//  Snp_addr = Orig_addr >> 3
// ============================================================
void ChiTransactionManager::process_rxsnp(uint32_t mst_idx,
                                          uint32_t txn_id,
                                          Addr_t addr,
                                          Timestamp_t req_time)
{
    std::lock_guard<std::mutex> lock(mtx_);

    MstKey key = {mst_idx, txn_id};

    if (outstanding_snoops_.count(key)) {
        LOG_DEBUG("[CHI MGR] WARN: duplicate rxsnp "
                  << " mst=" << get_mst_name(mst_idx) << " txnid=" << std::hex << txn_id << "\n");
    }

    Addr_t real_addr = (addr << 3);

    ChiOutstandingSnoop req{};
    req.key             = key;
    req.addr            = real_addr;
    req.expected_flits  = 4;
    req.req_time        = req_time;
    outstanding_snoops_[key] = req;

    LOG_DEBUG("[CHI MGR] SNP: mst=" << get_mst_name(mst_idx) 
              << " txnid=" << std::hex << txn_id 
              << " Snp addr=0x" << std::hex << addr 
              << " orig addr=0x" << std::hex << real_addr << std::dec
              << " time=" << req_time << "\n");
}


// ============================================================
// dump_outstanding_writes: for debug, display all info about
//   In-fly transaction
// ============================================================
void ChiTransactionManager::dump_outstanding_writes(uint32_t target_mst_idx) const
{
    LOG_ERROR("  [CHI MGR] Outstanding writes (Target mst=" << target_mst_idx << ":\n");
    if (outstanding_writes_.empty()) {
        LOG_ERROR("     (none writes record in system)\n");
        return;
    }
    
    for (const auto& kv : outstanding_writes_) {
        const auto& req = kv.second;
        LOG_ERROR("     - mst=" << get_mst_name(req.key.mst_idx)
                    << " txnid=" << req.key.txn_id 
                    << " addr=0x" << std::hex << req.addr << std::dec
                    << " opcode=0x" << std::hex << req.opcode << std::dec
                    << " dbid_valid=0x" << req.dbid_valid
                    << " dbid=" << req.dbid
                    << " exp_flits=" << req.expected_flits
                    << " acc_flits=" << req.accumulated_flits << "\n");
    }
}


