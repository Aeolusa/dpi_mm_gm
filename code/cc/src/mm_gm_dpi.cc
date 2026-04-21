// ============================================================
// file: dpi_interface.cpp
// SystemVerilog DPI-C 外部接口
// ============================================================
#include "transaction_manager.h"
#include <cstring>

static TransactionManager* g_mgr = nullptr;

extern "C" {

// ---- 初始化 ----
void refmodel_init(int num_masters, int strict_mode) {
    if (g_mgr) delete g_mgr;
    g_mgr = new TransactionManager(num_masters, strict_mode != 0);
}

// ---- 提交写请求并立即完成 (简化接口) ----
void refmodel_write(int          master_id,
                    long long    addr,
                    int          size,
                    int          burst_len,
                    const char*  data_hex,     // hex字符串形式传数据
                    const char*  byte_en_hex,
                    long long    req_time,
                    long long    resp_time)
{
    if (!g_mgr) return;

    Data_t data   = hex_to_bytes(data_hex);
    ByteEn_t be   = hex_to_byte_en(byte_en_hex, data.size());

    auto txn_id = g_mgr->submit_request(
        master_id, TxnType::WRITE, addr, size, burst_len,
        data, be, req_time);

    g_mgr->complete_transaction(master_id, txn_id, {}, resp_time);
}

// ---- 提交读请求并检查 ----
int refmodel_read_check(int          master_id,
                        long long    addr,
                        int          size,
                        int          burst_len,
                        const char*  resp_data_hex,
                        long long    req_time,
                        long long    resp_time)
{
    if (!g_mgr) return -1;

    Data_t resp_data = hex_to_bytes(resp_data_hex);
    ByteEn_t be(resp_data.size(), true);

    auto txn_id = g_mgr->submit_request(
        master_id, TxnType::READ, addr, size, burst_len,
        {}, be, req_time);

    auto report = g_mgr->complete_transaction(
        master_id, txn_id, resp_data, resp_time);

    return (report.result == CheckResult::PASS) ? 0 : 1;
}

// ---- 仿真结束 ----
void refmodel_finish() {
    if (g_mgr) {
        g_mgr->final_report();
        delete g_mgr;
        g_mgr = nullptr;
    }
}

} // extern "C"