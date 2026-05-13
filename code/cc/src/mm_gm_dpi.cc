// ============================================================
// file: src/mm_gm_dpi.cc
// SystemVerilog DPI-C 外部接口 (CHI)
//
// 接口说明：
//   - 所有函数新增 mst_idx 参数，区分不同BFM实例
//   - 数据通道传入原生 256-bit (svBitVecVal* = uint32_t[8])
//   - 读写事务均通过 process_txreq 路由，opcode 区分类型
//   - txreq 新增 is_sm 参数，由 BFM 参数 IS_SM 传入
//   - NCBWrData.txnid 实际为 dbid（CHI协议规定）
// ============================================================
#include "chi_transaction_manager.h"
#include "logger.h"
#include "utils.h"
#include <cstring>
#include <iostream>

static ChiTransactionManager* g_mgr = nullptr;

// 将 SV bit[255:0] (8x uint32_t, little-endian word) 解包为 32 字节数组
static void unpack_flit256(const uint32_t* sv_data, uint8_t* out_bytes)
{
    for (int w = 0; w < 8; ++w) {
        uint32_t word = sv_data[w];
        out_bytes[w*4 + 0] = (word      ) & 0xFF;
        out_bytes[w*4 + 1] = (word >>  8) & 0xFF;
        out_bytes[w*4 + 2] = (word >> 16) & 0xFF;
        out_bytes[w*4 + 3] = (word >> 24) & 0xFF;
    }
}

extern "C" {

// ---- 初始化 ----
//   strict_mode : 严格模式开关
//   log_level   : 0=QUIET, 1=ERROR(默认), 2=INFO, 3=DEBUG
//   log_file    : 日志文件路径（空字符串则不写文件）
void refmodel_init(int strict_mode, int log_level, const char* log_file) {
    // 配置日志系统
    LogLevel lvl = LogLevel::ERROR; // 默认
    switch (log_level) {
        case 0:  lvl = LogLevel::QUIET; break;
        case 1:  lvl = LogLevel::ERROR; break;
        case 2:  lvl = LogLevel::INFO;  break;
        case 3:  lvl = LogLevel::DEBUG; break;
        default: lvl = LogLevel::ERROR; break;
    }
    Logger::instance().set_level(lvl);

    // 配置日志文件输出
    if (log_file && std::strlen(log_file) > 0) {
        Logger::instance().enable_file_output(std::string(log_file));
    } else {
        Logger::instance().disable_file_output();
    }

    if (g_mgr) delete g_mgr;
    g_mgr = new ChiTransactionManager(strict_mode != 0);

    LOG_INFO("[REFMODEL] Initialized: strict=" << strict_mode
             << " log_level=" << log_level
             << " log_file=" << (log_file ? log_file : "(none)") << "\n");
}

// ---- 运行时动态切换日志级别 ----
void refmodel_set_log_level(int log_level) {
    LogLevel lvl = LogLevel::ERROR;
    switch (log_level) {
        case 0:  lvl = LogLevel::QUIET; break;
        case 1:  lvl = LogLevel::ERROR; break;
        case 2:  lvl = LogLevel::INFO;  break;
        case 3:  lvl = LogLevel::DEBUG; break;
        default: lvl = LogLevel::ERROR; break;
    }
    Logger::instance().set_level(lvl);
}

// ---- txreq: 读或写请求（opcode 区分） ----
//   mst_idx : BFM实例编号
//   txnid   : CHI txnid (12-bit, in SV passed as int)
//   addr    : 54-bit 地址
//   size    : CHI size 编码 (0..7)
//   opcode  : 4-bit REQOPCODE
//   secvec  : 4-bit cacheline 有效段向量
//   req_time: $time (ns)
//   is_sm   : 是否为 SM 类型 master（BFM 参数 IS_SM）
void dpi_chi_txreq(int mst_idx, int txnid, long long addr,
                   int size, int opcode, int secvec,
                   long long req_time, int is_sm)
{
    if (!g_mgr) return;
    g_mgr->process_txreq(
        static_cast<uint32_t>(mst_idx),
        static_cast<uint32_t>(txnid),
        static_cast<uint64_t>(addr),
        static_cast<uint32_t>(size),
        static_cast<uint32_t>(opcode),
        static_cast<uint32_t>(secvec),
        static_cast<uint64_t>(req_time),
        is_sm != 0
    );
}

// ---- rxrsp: DBIDResp / CompDBIDResp ----
//   txnid  : rxrsp.txnid（匹配 txreq.txnid）
//   opcode : 3-bit RSPOPCODE
//   dbid   : 12-bit DBID
void dpi_chi_rxrsp_dbid(int mst_idx, int txnid, int opcode, int dbid)
{
    if (!g_mgr) return;
    g_mgr->process_rxrsp_dbid(
        static_cast<uint32_t>(mst_idx),
        static_cast<uint32_t>(txnid),
        static_cast<uint32_t>(opcode),
        static_cast<uint32_t>(dbid)
    );
}

// ---- txdat: NCBWrData（写数据） ----
//   txnid    : 注意！这里传入的实际是 dbid 值（CHI协议 NCBWrData.txnid = DBID）
//   opcode   : 2-bit DATOPCODE
//   dataid   : 4-bit, 编码: 0/2/4/6 → 对应 cacheline 中 32B 段
//   data     : bit[255:0] → 以 8 x uint32_t 传入 (svBitVecVal)
//   be       : 32-bit byte enable (bit i → byte i)
//   data_cnt : SM 专用，表示本次事务总flit数；其他类型传0
void dpi_chi_txdat(int mst_idx, int txnid, int opcode,
                   int dataid, const uint32_t* data,
                   int be, int data_cnt)
{
    if (!g_mgr) return;
    uint8_t flit_bytes[32];
    unpack_flit256(data, flit_bytes);
    g_mgr->process_txdat(
        static_cast<uint32_t>(mst_idx),
        static_cast<uint32_t>(txnid),   // 实际为 dbid
        static_cast<uint32_t>(opcode),
        static_cast<uint32_t>(dataid),
        flit_bytes,
        static_cast<uint32_t>(be),
        static_cast<uint32_t>(data_cnt)
    );
}

// ---- rxdat: CompData（读返回数据） ----
//   txnid    : rxdat.txnid（匹配 txreq.txnid）
//   dataid   : 决定在cacheline中的偏移
//   data_cnt : 读流程通常不使用（传0）
void dpi_chi_rxdat(int mst_idx, int txnid, int opcode,
                   int dataid, const uint32_t* data,
                   int be, int data_cnt)
{
    if (!g_mgr) return;
    uint8_t flit_bytes[32];
    unpack_flit256(data, flit_bytes);
    g_mgr->process_rxdat(
        static_cast<uint32_t>(mst_idx),
        static_cast<uint32_t>(txnid),
        static_cast<uint32_t>(opcode),
        static_cast<uint32_t>(dataid),
        flit_bytes,
        static_cast<uint32_t>(be),
        static_cast<uint32_t>(data_cnt)
    );
}

// ---- 后门 hex 文件预加载 ----
//   filepath        : hex 文件路径
//   mem_width_bytes : 每行数据宽度（字节数），如 8 表示 64-bit
// 格式:
//   @80000000          ← hex 基地址
//   ffffffffaaaaaaaa   ← 每行 mem_width_bytes 字节的 hex 数据
// 返回 1=成功, 0=失败
int refmodel_preload_hex(const char* filepath, int mem_width_bytes) {
    if (!g_mgr) return 0;
    // 空路径：静默跳过，返回成功
    if (!filepath || std::strlen(filepath) == 0) return 1;
    bool ok = g_mgr->get_checker().preload_hex_file(
        std::string(filepath),
        static_cast<uint32_t>(mem_width_bytes)
    );
    return ok ? 1 : 0;
}

// ---- 仿真结束 ----
void refmodel_finish() {
    if (g_mgr) {
        auto stats = g_mgr->get_checker().get_stats();
        std::ostringstream oss;
        oss << "\n=== REFMODEL FINISHED ===\n"
            << "Total Writes: " << stats.total_writes << "\n"
            << "Total Reads : " << stats.total_reads  << "\n"
            << "Passes      : " << stats.passes       << "\n"
            << "Errors      : " << stats.errors       << "\n"
            << "=========================\n";
        LOG_ALWAYS(oss.str());
        Logger::instance().disable_file_output();
        delete g_mgr;
        g_mgr = nullptr;
    }
}

} // extern "C"