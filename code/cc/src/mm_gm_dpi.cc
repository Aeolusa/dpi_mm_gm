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
#include "refmodel_config.h"
#include "logger.h"
#include "utils.h"
#include <cstring>
#include <iostream>
#include <cstdlib>

static ChiTransactionManager* g_mgr = nullptr;
static RefModelConfig g_cfg;  // 全局配置（供其他模块查询）

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

// Extract bit [high:low]
static uint64_t extract_bits(const uint32_t* flit, int high, int low) {
    uint64_t val = 0;
    int bit_len = high - low + 1;
    if (bit_len <= 0 || bit_len > 64) return 0;
    for (int i = 0; i < bit_len; ++i) {
        int bit_idx = low + i;
        int word_idx = bit_idx / 32;
        int bit_in_word = bit_idx % 32;
        if ((flit[word_idx] >> bit_in_word) & 1) {
            val |= (1ULL << i);
        }
    }
    return val;
}

static void extract_bits_to_array(const uint32_t* flit, int high, int low, uint8_t* out_bytes, int out_len_bytes) {
    std::memset(out_bytes, 0, out_len_bytes);
    int bit_len = high - low + 1;
    for (int i = 0; i < bit_len; ++i) {
        if (i >= out_len_bytes * 8) break;
        int bit_idx = low + i;
        int word_idx = bit_idx / 32;
        int bit_in_word = bit_idx % 32;
        if ((flit[word_idx] >> bit_in_word) & 1) {
            out_bytes[i / 8] |= (1 << (i % 8));
        }
    }
}

#define REQ_TXNID_H 26
#define REQ_TXNID_L 15
#define REQ_OPCODE_H 30
#define REQ_OPCODE_L 27
#define REQ_SIZE_H 34
#define REQ_SIZE_L 31
#define REQ_SECVEC_H 38
#define REQ_SECVEC_L 35
#define REQ_ADDR_H 92
#define REQ_ADDR_L 39

#define RSP_TXNID_H 26
#define RSP_TXNID_L 15
#define RSP_OPCODE_H 29
#define RSP_OPCODE_L 27
#define RSP_DBID_H 46
#define RSP_DBID_L 35

#define TXDAT_TXNID_H 26
#define TXDAT_TXNID_L 15
#define TXDAT_OPCODE_H 28
#define TXDAT_OPCODE_L 27
#define TXDAT_DATAID_H 34
#define TXDAT_DATAID_L 31
#define TXDAT_BE_H 322
#define TXDAT_BE_L 291
#define TXDAT_DATA_H 290
#define TXDAT_DATA_L 35
#define TXDAT_DATA_CNT_H 325
#define TXDAT_DATA_CNT_L 324

#define RXDAT_TXNID_H 15
#define RXDAT_TXNID_L 4
#define RXDAT_OPCODE_H 17
#define RXDAT_OPCODE_L 16
#define RXDAT_DATAID_H 23
#define RXDAT_DATAID_L 20
#define RXDAT_BE_H 311
#define RXDAT_BE_L 280
#define RXDAT_DATA_H 279
#define RXDAT_DATA_L 24

// HST
#define HST_REQ_TXNID_H 37
#define HST_REQ_TXNID_L 26
#define HST_REQ_OPCODE_H 68
#define HST_REQ_OPCODE_L 62
#define HST_REQ_SIZE_H 72
#define HST_REQ_SIZE_L 69
#define HST_REQ_SECVEC_H 220
#define HST_REQ_SECVEC_L 217
#define HST_REQ_ADDR_H 127
#define HST_REQ_ADDR_L 73

#define HST_RSP_TXNID_H 37
#define HST_RSP_TXNID_L 26
#define HST_RSP_OPCODE_H 42
#define HST_RSP_OPCODE_L 38
#define HST_RSP_DBID_H 65
#define HST_RSP_DBID_L 54

#define HST_DAT_TXNID_H 37
#define HST_DAT_TXNID_L 26
#define HST_DAT_OPCODE_H 52
#define HST_DAT_OPCODE_L 49
#define HST_DAT_DATAID_H 97
#define HST_DAT_DATAID_L 92
#define HST_DAT_BE_H 134
#define HST_DAT_BE_L 103
#define HST_DAT_DATA_H 391
#define HST_DAT_DATA_L 135

#define HST_SNP_TXNID_H 26
#define HST_SNP_TXNID_L 15
#define HST_SNP_ADDR_H 103
#define HST_SNP_ADDR_L 55

extern "C" {

// ---- 初始化 ----
//   strict_mode : 严格模式开关
//   log_level   : 0=QUIET, 1=ERROR(默认), 2=INFO, 3=DEBUG
//   log_file    : 日志文件路径（空字符串则不写文件）
void refmodel_init(int strict_mode, int log_level, const char* log_file) {
    // 1. 尝试从环境变量 REFMODEL_CFG 读取配置文件路径，若无则默认使用 "refmodel.cfg"
    const char* env_cfg_path = std::getenv("REFMODEL_CFG");
    std::string cfg_path = env_cfg_path ? std::string(env_cfg_path) : "refmodel.cfg";
    g_cfg = load_config(cfg_path);

    // 2. 确定最终参数（配置文件优先，SV 传入参数作为 fallback）
    int    final_strict = g_cfg.loaded_from_file ? g_cfg.strict_mode : strict_mode;
    int    final_log_lv = g_cfg.loaded_from_file ? g_cfg.log_level   : log_level;
    std::string final_logf = g_cfg.loaded_from_file
        ? g_cfg.log_file
        : (log_file ? std::string(log_file) : "");
    uint64_t final_mask = g_cfg.addr_filter_mask;  // 仅来自配置文件（默认 0）

    // 3. 配置日志系统
    LogLevel lvl = LogLevel::ERROR;
    switch (final_log_lv) {
        case 0:  lvl = LogLevel::QUIET; break;
        case 1:  lvl = LogLevel::ERROR; break;
        case 2:  lvl = LogLevel::INFO;  break;
        case 3:  lvl = LogLevel::DEBUG; break;
        default: lvl = LogLevel::ERROR; break;
    }
    Logger::instance().set_level(lvl);

    if (!final_logf.empty()) {
        Logger::instance().enable_file_output(final_logf);
    } else {
        Logger::instance().disable_file_output();
    }

    // 4. 创建事务管理器
    if (g_mgr) delete g_mgr;
    g_mgr = new ChiTransactionManager(final_strict != 0, final_mask);

    // 5. 日志输出初始化信息
    std::ostringstream oss;
    oss << "[REFMODEL] Initialized:"
        << " strict=" << final_strict
        << " log_level=" << final_log_lv
        << " log_file=" << (final_logf.empty() ? "(none)" : final_logf)
        << " addr_filter_mask=0x" << std::hex << final_mask << std::dec;
    if (g_cfg.loaded_from_file) {
        oss << " (from refmodel.cfg)";
    } else {
        oss << " (from SV args, no config file)";
    }
    oss << "\n";
    LOG_INFO(oss.str());

    // 6. 自动预加载 hex（如配置文件指定）
    if (g_cfg.loaded_from_file && !g_cfg.preload_hex_file.empty()) {
        bool ok = g_mgr->get_checker().preload_hex_file(
            g_cfg.preload_hex_file,
            static_cast<uint32_t>(g_cfg.preload_hex_width));
        if (!ok) {
            LOG_ERROR("[REFMODEL] Failed to preload hex: " << g_cfg.preload_hex_file << "\n");
        }
    }

    g_mgr->clr_error();
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
                   long long req_time, int is_sm, int soft_ctrl,
                   int is_fabric_chi, const uint32_t* flit)
{
    if (!g_mgr) return;
    if (g_cfg.enable_soft_ctrl) {
        g_mgr->check_soft_ctrl(soft_ctrl != 0);
    }

    if (soft_ctrl) {
        if (is_fabric_chi) {
            txnid = extract_bits(flit, HST_REQ_TXNID_H, HST_REQ_TXNID_L);
            opcode = extract_bits(flit, HST_REQ_OPCODE_H, HST_REQ_OPCODE_L);
            size = extract_bits(flit, HST_REQ_SIZE_H, HST_REQ_SIZE_L);
            secvec = extract_bits(flit, HST_REQ_SECVEC_H, HST_REQ_SECVEC_L);
            addr = extract_bits(flit, HST_REQ_ADDR_H, HST_REQ_ADDR_L);
        } else {
            txnid = extract_bits(flit, REQ_TXNID_H, REQ_TXNID_L);
            opcode = extract_bits(flit, REQ_OPCODE_H, REQ_OPCODE_L);
            size = extract_bits(flit, REQ_SIZE_H, REQ_SIZE_L);
            secvec = extract_bits(flit, REQ_SECVEC_H, REQ_SECVEC_L);
            addr = extract_bits(flit, REQ_ADDR_H, REQ_ADDR_L);
        }
    }

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
void dpi_chi_rxrsp_dbid(int mst_idx, int txnid, int opcode, int dbid, int soft_ctrl, int is_fabric_chi, const uint32_t* flit)
{
    if (!g_mgr) return;
    g_mgr->check_soft_ctrl(soft_ctrl != 0);
    if (soft_ctrl) {
        if (is_fabric_chi) {
            txnid = extract_bits(flit, HST_RSP_TXNID_H, HST_RSP_TXNID_L);
            opcode = extract_bits(flit, HST_RSP_OPCODE_H, HST_RSP_OPCODE_L);
            dbid = extract_bits(flit, HST_RSP_DBID_H, HST_RSP_DBID_L);
        } else {
            txnid = extract_bits(flit, RSP_TXNID_H, RSP_TXNID_L);
            opcode = extract_bits(flit, RSP_OPCODE_H, RSP_OPCODE_L);
            dbid = extract_bits(flit, RSP_DBID_H, RSP_DBID_L);
        }
    }
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
                   int be, int data_cnt, int soft_ctrl, int is_fabric_chi, const uint32_t* flit)
{
    if (!g_mgr) return;
    g_mgr->check_soft_ctrl(soft_ctrl != 0);
    uint8_t flit_bytes[32];
    if (soft_ctrl) {
        if (is_fabric_chi) {
            txnid = extract_bits(flit, HST_DAT_TXNID_H, HST_DAT_TXNID_L);
            opcode = extract_bits(flit, HST_DAT_OPCODE_H, HST_DAT_OPCODE_L);
            dataid = extract_bits(flit, HST_DAT_DATAID_H, HST_DAT_DATAID_L);
            be = extract_bits(flit, HST_DAT_BE_H, HST_DAT_BE_L);
            data_cnt = extract_bits(flit, TXDAT_DATA_CNT_H, TXDAT_DATA_CNT_L);
            extract_bits_to_array(flit, HST_DAT_DATA_H, HST_DAT_DATA_L, flit_bytes, 32);
        } else {
            txnid = extract_bits(flit, TXDAT_TXNID_H, TXDAT_TXNID_L);
            opcode = extract_bits(flit, TXDAT_OPCODE_H, TXDAT_OPCODE_L);
            dataid = extract_bits(flit, TXDAT_DATAID_H, TXDAT_DATAID_L);
            be = extract_bits(flit, TXDAT_BE_H, TXDAT_BE_L);
            data_cnt = extract_bits(flit, TXDAT_DATA_CNT_H, TXDAT_DATA_CNT_L);
            extract_bits_to_array(flit, TXDAT_DATA_H, TXDAT_DATA_L, flit_bytes, 32);
        }
    } else {
        unpack_flit256(data, flit_bytes);
    }

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
                   int be, int data_cnt, int soft_ctrl, int is_fabric_chi, const uint32_t* flit)
{
    if (!g_mgr) return;
    g_mgr->check_soft_ctrl(soft_ctrl != 0);
    uint8_t flit_bytes[32];
    if (soft_ctrl) {
        if (is_fabric_chi) {
            txnid = extract_bits(flit, HST_DAT_TXNID_H, HST_DAT_TXNID_L);
            opcode = extract_bits(flit, HST_DAT_OPCODE_H, HST_DAT_OPCODE_L);
            dataid = extract_bits(flit, HST_DAT_DATAID_H, HST_DAT_DATAID_L);
            be = extract_bits(flit, HST_DAT_BE_H, HST_DAT_BE_L);
            data_cnt = 0;
            extract_bits_to_array(flit, HST_DAT_DATA_H, HST_DAT_DATA_L, flit_bytes, 32);
        } else {
            txnid = extract_bits(flit, RXDAT_TXNID_H, RXDAT_TXNID_L);
            opcode = extract_bits(flit, RXDAT_OPCODE_H, RXDAT_OPCODE_L);
            dataid = extract_bits(flit, RXDAT_DATAID_H, RXDAT_DATAID_L);
            be = extract_bits(flit, RXDAT_BE_H, RXDAT_BE_L);
            data_cnt = 0;
            extract_bits_to_array(flit, RXDAT_DATA_H, RXDAT_DATA_L, flit_bytes, 32);
        }
    } else {
        unpack_flit256(data, flit_bytes);
    }

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

void dpi_chi_rxsnp( int mst_idx, int txnid, long long addr, 
                    long long req_time, int soft_ctrl, int is_hst, const uint32_t* flit)
{
    if (!g_mgr) return;
    g_mgr->check_soft_ctrl(soft_ctrl != 0);
    if (soft_ctrl) {
        if (is_hst) {
            txnid = extract_bits(flit, HST_SNP_TXNID_H, HST_SNP_TXNID_L);
            addr = extract_bits(flit, HST_SNP_ADDR_H, HST_SNP_ADDR_L);
        }
    }

    g_mgr->process_rxsnp(
        static_cast<uint32_t>(mst_idx),
        static_cast<uint32_t>(txnid),
        static_cast<uint64_t>(addr),
        static_cast<uint64_t>(req_time)
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
            << "Total Writes        : " << stats.total_writes << "\n"
            << "Total Reads         : " << stats.total_reads  << "\n"
            << "Passes              : " << stats.passes       << "\n"
            << "Errors              : " << stats.errors       << "\n"
            << "Filtered Mismatches : " << stats.filtered_mismatches << "\n"
            << "Warnings            : " << stats.warnings     << "\n"
            << "=========================\n";
        LOG_ALWAYS(oss.str());
        Logger::instance().disable_file_output();
        delete g_mgr;
        g_mgr = nullptr;
    }
}

} // extern "C"

