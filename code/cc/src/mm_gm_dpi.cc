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
                   const uint32_t* data_bits, uint32_t byte_en_bits,
                   int offset, long long resp_time)
{
    if (!g_mgr) return;
    
    // CHI data flit is 256 bits (32 bytes) -> 8 uint32_t
    Data_t data(32);
    for (int i = 0; i < 8; ++i) {
        uint32_t word = data_bits[i];
        data[i*4 + 0] = word & 0xFF;
        data[i*4 + 1] = (word >> 8) & 0xFF;
        data[i*4 + 2] = (word >> 16) & 0xFF;
        data[i*4 + 3] = (word >> 24) & 0xFF;
    }
    
    // CHI byte enable is 32 bits -> 1 uint32_t
    ByteEn_t be(32);
    for (int i = 0; i < 32; ++i) {
        be[i] = (byte_en_bits & (1U << i)) != 0;
    }

    g_mgr->process_txdat_flit(src_id, tgt_id, dbid, data, be, offset, resp_time);
}

// ---- 简化的读取和检查（如果不需要读流程追踪） ----
int dpi_chi_read_check(int src_id, int tgt_id, int txn_id,
                       long long addr, int size, int burst_len,
                       const uint32_t* resp_data_bits,
                       long long req_time, long long resp_time)
{
    if (!g_mgr) return -1;
    
    int total_bytes = size * burst_len;
    Data_t data(total_bytes);
    int num_words = (total_bytes + 3) / 4;
    for (int i = 0; i < num_words; ++i) {
        uint32_t word = resp_data_bits[i];
        for (int j = 0; j < 4; ++j) {
            if (i * 4 + j < total_bytes) {
                data[i * 4 + j] = (word >> (j * 8)) & 0xFF;
            }
        }
    }
    
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