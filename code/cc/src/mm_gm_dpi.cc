// ============================================================
// file: src/mm_gm_dpi.cc
// SystemVerilog DPI-C 外部接口 (CHI)
// ============================================================
#include "chi_transaction_manager.h"
#include "utils.h"
#include <cstring>
#include <iostream>

static ChiTransactionManager* g_mgr = nullptr;

extern "C" {

// ---- 初始化 ----
void refmodel_init(int strict_mode) {
    if (g_mgr) delete g_mgr;
    g_mgr = new ChiTransactionManager(strict_mode != 0);
}

// ---- txreq 收到写请求 ----
void dpi_chi_txreq_write(int src_id, int tgt_id, int txn_id,
                         long long addr, int size, int burst_len,
                         long long req_time)
{
    if (!g_mgr) return;
    g_mgr->process_txreq_flit(src_id, tgt_id, txn_id, addr, size, burst_len, req_time);
}

// ---- rxrsp 收到写响应 ----
void dpi_chi_rxrsp_dbid(int src_id, int tgt_id, int txn_id, int dbid)
{
    if (!g_mgr) return;
    g_mgr->process_rxrsp_flit(src_id, tgt_id, txn_id, dbid);
}

// ---- txdat 收到写数据 ----
void dpi_chi_txdat(int src_id, int tgt_id, int dbid,
                   const char* data_hex, const char* byte_en_hex,
                   int offset, long long resp_time)
{
    if (!g_mgr) return;
    Data_t data = hex_to_bytes(data_hex);
    ByteEn_t be = hex_to_byte_en(byte_en_hex, data.size());
    g_mgr->process_txdat_flit(src_id, tgt_id, dbid, data, be, offset, resp_time);
}

// ---- 简化的读取和检查（如果不需要读流程追踪） ----
int dpi_chi_read_check(int src_id, int tgt_id, int txn_id,
                       long long addr, int size, int burst_len,
                       const char* resp_data_hex,
                       long long req_time, long long resp_time)
{
    if (!g_mgr) return -1;
    Data_t data = hex_to_bytes(resp_data_hex);
    g_mgr->process_read_completed(src_id, tgt_id, txn_id, addr, size, burst_len, data, req_time, resp_time);
    
    // 由于 process_read_completed 内部打印了错误信息，这里简化返回0
    // 如果你的 SV 环境需要根据返回值决定是否 error，可以修改 process_read_completed 返回 bool
    return 0;
}

// ---- 仿真结束 ----
void refmodel_finish() {
    if (g_mgr) {
        auto stats = g_mgr->get_checker().get_stats();
        std::cout << "\n=== REFMODEL FINISHED ===\n"
                  << "Total Writes: " << stats.total_writes << "\n"
                  << "Total Reads : " << stats.total_reads << "\n"
                  << "Passes      : " << stats.passes << "\n"
                  << "Errors      : " << stats.errors << "\n"
                  << "=========================\n";
        delete g_mgr;
        g_mgr = nullptr;
    }
}

} // extern "C"