// ============================================================
// file: src/chi_transaction_manager.cc
// ============================================================
#include "chi_transaction_manager.h"
#include <iostream>

ChiTransactionManager::ChiTransactionManager(bool strict_mode)
    : checker_(strict_mode), global_seq_(1)
{
}

void ChiTransactionManager::process_txreq_flit(uint32_t src_id, uint32_t tgt_id, uint32_t txn_id,
                                               Addr_t addr, uint32_t size, uint32_t burst_len,
                                               Timestamp_t req_time)
{
    TxnKey key = {src_id, tgt_id, txn_id};
    
    // 如果已经存在，可能是重复发请求或者异常
    if (outstanding_writes_.find(key) != outstanding_writes_.end()) {
        std::cerr << "[CHI MGR] WARNING: txreq outstanding entry already exists for SRC=" 
                  << src_id << " TGT=" << tgt_id << " TXN=" << txn_id << "\n";
    }

    ChiOutstandingWrite req;
    req.src_id    = src_id;
    req.tgt_id    = tgt_id;
    req.txn_id    = txn_id;
    req.addr      = addr;
    req.size      = size;
    req.burst_len = burst_len;
    req.req_time  = req_time;
    req.dbid_valid = false;
    
    // 预分配缓冲区
    uint32_t total_bytes = size * burst_len;
    req.data.resize(total_bytes, 0);
    req.byte_enable.resize(total_bytes, false);
    req.accumulated_bytes = 0;

    outstanding_writes_[key] = std::move(req);
}

void ChiTransactionManager::process_rxrsp_flit(uint32_t src_id, uint32_t tgt_id, uint32_t txn_id,
                                               uint32_t dbid)
{
    // rxrsp: 从 Home(tgt) 发给 Requester(src)
    // 所以这里的 src_id 是 Home, tgt_id 是 Requester
    // 而我们要找的 txreq 表项的 key 应该是: Req -> Home
    TxnKey key = {tgt_id, src_id, txn_id};
    
    auto it = outstanding_writes_.find(key);
    if (it == outstanding_writes_.end()) {
        std::cerr << "[CHI MGR] ERROR: rxrsp cannot find outstanding request SRC=" 
                  << tgt_id << " TGT=" << src_id << " TXN=" << txn_id << "\n";
        return;
    }
    
    it->second.dbid = dbid;
    it->second.dbid_valid = true;
    
    // txdat 是由 Requester 发给 Home 的，所以 txdat 的 src_id=Req, tgt_id=Home
    // 我们用 (Req, Home, dbid) 建立映射
    TxnKey dbid_key = {tgt_id, src_id, dbid};
    dbid_to_txnkey_[dbid_key] = key;
}

void ChiTransactionManager::process_txdat_flit(uint32_t src_id, uint32_t tgt_id, uint32_t dbid,
                                               const Data_t& data_flit, const ByteEn_t& be_flit,
                                               uint32_t offset, Timestamp_t resp_time)
{
    TxnKey dbid_key = {src_id, tgt_id, dbid};
    auto dbid_it = dbid_to_txnkey_.find(dbid_key);
    if (dbid_it == dbid_to_txnkey_.end()) {
        std::cerr << "[CHI MGR] ERROR: txdat cannot find mapped request for DBID=" 
                  << dbid << " SRC=" << src_id << " TGT=" << tgt_id << "\n";
        return;
    }
    
    TxnKey orig_key = dbid_it->second;
    auto req_it = outstanding_writes_.find(orig_key);
    if (req_it == outstanding_writes_.end()) {
        std::cerr << "[CHI MGR] ERROR: txdat found dbid mapping but no request for DBID=" 
                  << dbid << "\n";
        return;
    }
    
    ChiOutstandingWrite& req = req_it->second;
    
    // 将 data_flit 和 be_flit 拷贝到请求的数据区
    uint32_t flit_len = data_flit.size();
    if (offset + flit_len > req.data.size()) {
        std::cerr << "[CHI MGR] ERROR: txdat offset+length exceeds transaction total size.\n";
        flit_len = req.data.size() - offset; // 截断防越界
    }
    
    for (uint32_t i = 0; i < flit_len; ++i) {
        req.data[offset + i] = data_flit[i];
        if (i < be_flit.size()) {
            req.byte_enable[offset + i] = be_flit[i];
        }
    }
    
    req.accumulated_bytes += flit_len;
    
    // 检查是否所有数据都已经收集完毕
    if (req.accumulated_bytes >= req.size * req.burst_len) {
        // 构造 Transaction 并丢给 ConsistencyChecker
        Transaction txn;
        txn.txn_id      = req.txn_id;
        txn.global_seq  = global_seq_++;
        txn.src_id      = req.src_id;
        txn.tgt_id      = req.tgt_id;
        txn.dbid        = req.dbid;
        txn.type        = TxnType::WRITE;
        txn.addr        = req.addr;
        txn.size        = req.size;
        txn.burst_len   = req.burst_len;
        txn.data        = std::move(req.data);
        txn.byte_enable = std::move(req.byte_enable);
        txn.req_time    = req.req_time;
        txn.resp_time   = resp_time;
        txn.status      = TxnStatus::COMPLETED;
        
        checker_.process_write(txn);
        
        // 释放表项
        dbid_to_txnkey_.erase(dbid_it);
        outstanding_writes_.erase(req_it);
    }
}

void ChiTransactionManager::process_read_completed(uint32_t src_id, uint32_t tgt_id, uint32_t txn_id,
                                                   Addr_t addr, uint32_t size, uint32_t burst_len,
                                                   const Data_t& data, Timestamp_t req_time, Timestamp_t resp_time)
{
    Transaction txn;
    txn.txn_id      = txn_id;
    txn.global_seq  = global_seq_++;
    txn.src_id      = src_id;
    txn.tgt_id      = tgt_id;
    txn.dbid        = 0;
    txn.type        = TxnType::READ;
    txn.addr        = addr;
    txn.size        = size;
    txn.burst_len   = burst_len;
    txn.data        = data;
    txn.req_time    = req_time;
    txn.resp_time   = resp_time;
    txn.status      = TxnStatus::COMPLETED;
    
    CheckReport report = checker_.process_read(txn);
    if (report.result != CheckResult::PASS) {
        std::cerr << "\n" << report.message << "\n";
    }
}
