// ============================================================
// file: src/chi_transaction_manager.cc
// CHI 事务管理器实现
// ============================================================
#include "chi_transaction_manager.h"
#include <iostream>
#include <cstring>
#include <cassert>

// ============================================================
// 构造
// ============================================================
ChiTransactionManager::ChiTransactionManager(bool strict_mode)
    : checker_(strict_mode), global_seq_(1)
{}

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
    txn.size        = CHI_CL_BYTES;   // 固定128字节缓冲
    txn.burst_len   = 1;
    txn.data        = std::move(data);
    txn.byte_enable = std::move(byte_en);
    txn.req_time    = req.req_time;
    txn.resp_time   = resp_time;
    txn.status      = TxnStatus::COMPLETED;

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
    txn.size        = CHI_CL_BYTES;
    txn.burst_len   = 1;
    txn.data        = std::move(data);
    txn.byte_enable = std::move(byte_en);
    txn.req_time    = req.req_time;
    txn.resp_time   = resp_time;
    txn.status      = TxnStatus::COMPLETED;

    CheckReport report = checker_.process_read(txn);
    if (report.result != CheckResult::PASS) {
        std::cerr << "\n" << report.message << "\n";
    }
}

// ============================================================
// process_txreq: 由 txreq_flitv 触发
//   opcode 区分 RdNoSnp / WrNoSnp
// ============================================================
void ChiTransactionManager::process_txreq(uint32_t mst_idx, uint32_t txn_id,
                                           Addr_t addr, uint32_t size,
                                           uint32_t opcode, uint32_t secvec,
                                           Timestamp_t req_time)
{
    MstKey key = {mst_idx, txn_id};
    uint32_t exp_flits = secvec_to_flits(secvec);
    if (exp_flits == 0) exp_flits = 1; // 至少1笔（防御）

    if (chi_is_read_opcode(opcode)) {
        // ---- 读请求 ----
        if (outstanding_reads_.count(key)) {
            std::cerr << "[CHI MGR] WARN: duplicate txreq read "
                      << "mst=" << mst_idx << " txnid=" << txn_id << "\n";
        }
        ChiOutstandingRead req{};
        req.key             = key;
        req.addr            = addr;
        req.opcode          = opcode;
        req.secvec          = secvec;
        req.expected_flits  = exp_flits;
        req.req_time        = req_time;
        outstanding_reads_[key] = req;

    } else if (chi_is_write_opcode(opcode)) {
        // ---- 写请求 ----
        if (outstanding_writes_.count(key)) {
            std::cerr << "[CHI MGR] WARN: duplicate txreq write "
                      << "mst=" << mst_idx << " txnid=" << txn_id << "\n";
        }
        ChiOutstandingWrite req{};
        req.key             = key;
        req.addr            = addr;
        req.opcode          = opcode;
        req.secvec          = secvec;
        req.expected_flits  = exp_flits;
        req.req_time        = req_time;
        outstanding_writes_[key] = req;

    } else {
        std::cerr << "[CHI MGR] WARN: unknown txreq opcode=0x"
                  << std::hex << opcode << std::dec
                  << " mst=" << mst_idx << " txnid=" << txn_id << "\n";
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
    MstKey key = {mst_idx, txn_id};
    auto it = outstanding_writes_.find(key);
    if (it == outstanding_writes_.end()) {
        std::cerr << "[CHI MGR] ERROR: rxrsp cannot find write "
                  << "mst=" << mst_idx << " txnid=" << txn_id << "\n";
        return;
    }

    it->second.dbid       = dbid;
    it->second.dbid_valid = true;

    // 建立逆向映射: {mst_idx, dbid} → txnid
    // txdat 到来时使用 txdat.txnid = dbid
    MstKey dbid_key = {mst_idx, dbid};
    dbid_to_txnid_[dbid_key] = txn_id;
}

// ============================================================
// process_txdat: 由 txdat_flitv 触发 (NCBWrData)
//   注意：txdat.txnid 字段实际携带的是 dbid！
//         即 NCBWrData.txnid = DBIDResp.dbid
// ============================================================
void ChiTransactionManager::process_txdat(uint32_t mst_idx,
                                           uint32_t txn_id_as_dbid,
                                           uint32_t opcode,
                                           uint32_t dataid,
                                           const uint8_t* flit_data,
                                           uint32_t be_mask,
                                           uint32_t data_cnt)
{
    // 用 (mst_idx, dbid) 找回原始 txnid
    MstKey dbid_key = {mst_idx, txn_id_as_dbid};
    auto dbid_it = dbid_to_txnid_.find(dbid_key);
    if (dbid_it == dbid_to_txnid_.end()) {
        std::cerr << "[CHI MGR] ERROR: txdat cannot find dbid mapping "
                  << "mst=" << mst_idx << " dbid=" << txn_id_as_dbid << "\n";
        return;
    }

    uint32_t orig_txnid = dbid_it->second;
    MstKey orig_key = {mst_idx, orig_txnid};
    auto req_it = outstanding_writes_.find(orig_key);
    if (req_it == outstanding_writes_.end()) {
        std::cerr << "[CHI MGR] ERROR: txdat found dbid map but no write "
                  << "mst=" << mst_idx << " txnid=" << orig_txnid << "\n";
        return;
    }

    ChiOutstandingWrite& req = req_it->second;

    // SM 类型特性：data_cnt 覆盖 expected_flits（仅第一笔有效时设置）
    if (data_cnt > 0 && !req.use_data_cnt) {
        req.expected_flits = data_cnt;
        req.use_data_cnt   = true;
    }

    // 按 dataid[1:0] 确定数据在 128B 缓冲中的偏移
    uint32_t byte_offset = dataid_to_offset(dataid);
    if (byte_offset + CHI_FLIT_BYTES > CHI_CL_BYTES) {
        std::cerr << "[CHI MGR] ERROR: txdat dataid=" << dataid
                  << " yields out-of-range offset=" << byte_offset << "\n";
        return;
    }

    // 填入数据和 byte enable
    for (uint32_t i = 0; i < CHI_FLIT_BYTES; ++i) {
        req.data_buf[byte_offset + i] = flit_data[i];
        req.be_buf  [byte_offset + i] = (be_mask >> i) & 0x1;
    }
    req.accumulated_flits++;

    // 检查是否收齐所有 flit
    if (req.accumulated_flits >= req.expected_flits) {
        Timestamp_t resp_time = 0; // txdat 无时间戳，使用0（或可扩展）
        finalize_write(req, resp_time);

        // 清理映射
        dbid_to_txnid_.erase(dbid_it);
        outstanding_writes_.erase(req_it);
    }
}

// ============================================================
// process_rxdat: 由 rxdat_flitv 触发 (CompData)
//   rxdat.txnid = txreq.txnid（读事务）
// ============================================================
void ChiTransactionManager::process_rxdat(uint32_t mst_idx,
                                           uint32_t txn_id,
                                           uint32_t opcode,
                                           uint32_t dataid,
                                           const uint8_t* flit_data,
                                           uint32_t be_mask,
                                           uint32_t data_cnt)
{
    MstKey key = {mst_idx, txn_id};
    auto it = outstanding_reads_.find(key);
    if (it == outstanding_reads_.end()) {
        std::cerr << "[CHI MGR] ERROR: rxdat cannot find read "
                  << "mst=" << mst_idx << " txnid=" << txn_id << "\n";
        return;
    }

    ChiOutstandingRead& req = it->second;

    // 按 dataid[1:0] 放置数据
    uint32_t byte_offset = dataid_to_offset(dataid);
    if (byte_offset + CHI_FLIT_BYTES > CHI_CL_BYTES) {
        std::cerr << "[CHI MGR] ERROR: rxdat dataid=" << dataid
                  << " out-of-range offset=" << byte_offset << "\n";
        return;
    }

    for (uint32_t i = 0; i < CHI_FLIT_BYTES; ++i) {
        req.data_buf[byte_offset + i] = flit_data[i];
        req.be_buf  [byte_offset + i] = (be_mask >> i) & 0x1;
    }
    req.accumulated_flits++;

    if (req.accumulated_flits >= req.expected_flits) {
        // 使用当前仿真时间（暂用0，txdat也一样；可从DPI侧传入）
        finalize_read(req, 0);
        outstanding_reads_.erase(it);
    }
}
