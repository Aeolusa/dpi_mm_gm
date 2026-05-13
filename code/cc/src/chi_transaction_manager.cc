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
        LOG_ERROR("\n" << report.message << "\n");
    }
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

    if (chi_is_read_opcode(opcode)) {
        // ---- 读请求：所有类型均使用 size 决定 flit 数 ----
        if (outstanding_reads_.count(key)) {
            LOG_DEBUG("[CHI MGR] WARN: duplicate txreq read "
                      << "mst=" << mst_idx << " txnid=" << txn_id << "\n");
        }
        uint32_t exp_flits = size_to_flits(size);

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
            LOG_DEBUG("[CHI MGR] WARN: duplicate txreq write "
                      << "mst=" << mst_idx << " txnid=" << txn_id << "\n");
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
        req.is_sm           = is_sm;
        req.expected_flits  = exp_flits;
        req.req_time        = req_time;
        outstanding_writes_[key] = req;

    } else {
        LOG_DEBUG("[CHI MGR] WARN: unknown txreq opcode=0x"
                  << std::hex << opcode << std::dec
                  << " mst=" << mst_idx << " txnid=" << txn_id << "\n");
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

    MstKey key = {mst_idx, txn_id};
    auto it = outstanding_writes_.find(key);
    if (it == outstanding_writes_.end()) {
        LOG_ERROR("[CHI MGR] ERROR: rxrsp cannot find write "
                  << "mst=" << mst_idx << " txnid=" << txn_id << "\n");
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
    std::lock_guard<std::mutex> lock(mtx_);

    // 用 (mst_idx, dbid) 找回原始 txnid
    MstKey dbid_key = {mst_idx, txn_id_as_dbid};
    auto dbid_it = dbid_to_txnid_.find(dbid_key);
    if (dbid_it == dbid_to_txnid_.end()) {
        LOG_ERROR("[CHI MGR] ERROR: txdat cannot find dbid mapping "
                  << "mst=" << mst_idx << " dbid=" << txn_id_as_dbid << "\n");
        return;
    }

    uint32_t orig_txnid = dbid_it->second;
    MstKey orig_key = {mst_idx, orig_txnid};
    auto req_it = outstanding_writes_.find(orig_key);
    if (req_it == outstanding_writes_.end()) {
        LOG_ERROR("[CHI MGR] ERROR: txdat found dbid map but no write "
                  << "mst=" << mst_idx << " txnid=" << orig_txnid << "\n");
        return;
    }

    ChiOutstandingWrite& req = req_it->second;

    // SM 写事务：收到首笔 txdat 时，用 data_cnt 覆盖 expected_flits
    if (req.is_sm && data_cnt > 0 && !req.data_cnt_applied) {
        req.expected_flits   = data_cnt;
        req.data_cnt_applied = true;
    }

    // 按 dataid 确定数据在 128B 缓冲中的偏移
    uint32_t byte_offset = dataid_to_offset(dataid);
    if (byte_offset + CHI_FLIT_BYTES > CHI_CL_BYTES) {
        LOG_ERROR("[CHI MGR] ERROR: txdat dataid=" << dataid
                  << " yields out-of-range offset=" << byte_offset << "\n");
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
                  << "mst=" << mst_idx << " txnid=" << txn_id << "\n");
        return;
    }

    ChiOutstandingRead& req = it->second;

    // 按 dataid 放置数据
    uint32_t byte_offset = dataid_to_offset(dataid);
    if (byte_offset + CHI_FLIT_BYTES > CHI_CL_BYTES) {
        LOG_ERROR("[CHI MGR] ERROR: rxdat dataid=" << dataid
                  << " out-of-range offset=" << byte_offset << "\n");
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
