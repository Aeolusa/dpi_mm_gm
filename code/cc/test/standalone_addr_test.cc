// ============================================================
// file: test/standalone_addr_test.cc
// 54-bit 地址高位截断诊断 — 独立测试（无 GTest 依赖）
//
// 编译方式（服务器上）:
//   g++ -std=c++17 -I../include \
//       ../src/chi_transaction_manager.cc \
//       ../src/consistency_check.cc \
//       ../src/shadow_memory.cc \
//       ../src/logger.cc \
//       ../src/utils.cc \
//       standalone_addr_test.cc \
//       -o addr_test -lpthread
//   ./addr_test
//
// 诊断逻辑:
//   全 PASS → C++ 软件侧无截断 → 问题在硬件/SV 侧
//   任何 FAIL → C++ 软件侧存在截断缺陷
// ============================================================
#include "chi_transaction_manager.h"
#include "refmodel_config.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <cinttypes>

// ---- 极简测试框架 ----
static int g_total = 0, g_pass = 0, g_fail = 0;
static const char* g_current_test = nullptr;

#define TEST_BEGIN(name)                                           \
    do {                                                           \
        g_current_test = name;                                     \
        g_total++;                                                 \
        printf("\n[TEST] %s ...\n", name);                         \
    } while (0)

#define ASSERT_TRUE(cond, msg)                                     \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
            g_fail++;                                              \
            return;                                                \
        }                                                          \
    } while (0)

#define ASSERT_EQ(a, b, msg)                                       \
    do {                                                           \
        auto _a = (a); auto _b = (b);                              \
        if (_a != _b) {                                            \
            printf("  [FAIL] %s:%d: %s (got %" PRIu64 ", expected %" PRIu64 ")\n", \
                   __FILE__, __LINE__, msg,                        \
                   (uint64_t)_a, (uint64_t)_b);                    \
            g_fail++;                                              \
            return;                                                \
        }                                                          \
    } while (0)

#define ASSERT_NE(a, b, msg)                                       \
    do {                                                           \
        if ((a) == (b)) {                                          \
            printf("  [FAIL] %s:%d: %s (both = 0x%" PRIx64 ")\n", \
                   __FILE__, __LINE__, msg, (uint64_t)(a));        \
            g_fail++;                                              \
            return;                                                \
        }                                                          \
    } while (0)

#define TEST_PASS()                                                \
    do {                                                           \
        g_pass++;                                                  \
        printf("  [PASS] %s\n", g_current_test);                   \
    } while (0)

// ---- 辅助函数 ----
static std::array<uint8_t, 32> make_flit(uint8_t fill) {
    std::array<uint8_t, 32> f;
    f.fill(fill);
    return f;
}

// 完整的单 flit 写事务
static void do_write(ChiTransactionManager& mgr,
                     uint32_t mst, uint32_t txnid, Addr_t addr,
                     uint32_t dbid, uint8_t fill, Timestamp_t t = 100) {
    printf("  [TEST_DEBUG] do_write: calling process_txreq(mst=%u, txnid=%u, addr=0x%llx)...\n",
           mst, txnid, (unsigned long long)addr);
    mgr.process_txreq(mst, txnid, addr, 5,
                      static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull),
                      0x1, t, false);

    printf("  [TEST_DEBUG] do_write: calling process_rxrsp_dbid(mst=%u, txnid=%u, dbid=%u)...\n",
           mst, txnid, dbid);
    mgr.process_rxrsp_dbid(mst, txnid, 0, dbid, /*srcid=*/0);

    printf("  [TEST_DEBUG] do_write: calling process_txdat(mst=%u, dbid=%u, fill=0x%02x)...\n",
           mst, dbid, fill);
    auto wd = make_flit(fill);
    mgr.process_txdat(mst, dbid, 0, 0, wd.data(), 0xFFFFFFFF, 0, /*tgtid=*/0);
    printf("  [TEST_DEBUG] do_write completed.\n");
}

// 完整的单 flit 读事务
static void do_read(ChiTransactionManager& mgr,
                    uint32_t mst, uint32_t txnid, Addr_t addr,
                    uint8_t expected_fill, Timestamp_t t = 200) {
    printf("  [TEST_DEBUG] do_read: calling process_txreq(mst=%u, txnid=%u, addr=0x%llx)...\n",
           mst, txnid, (unsigned long long)addr);
    mgr.process_txreq(mst, txnid, addr, 5,
                      static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                      0x1, t, false);

    printf("  [TEST_DEBUG] do_read: calling process_rxdat(mst=%u, txnid=%u, expected_fill=0x%02x)...\n",
           mst, txnid, expected_fill);
    auto rd = make_flit(expected_fill);
    mgr.process_rxdat(mst, txnid, 0, 0, rd.data(), 0xFFFFFFFF, 0);
    printf("  [TEST_DEBUG] do_read completed.\n");
}

// ============================================================
// Test 1: 高位别名检测（最核心）
//   两个地址仅 bit[53:48] 不同，低 48 位完全相同。
//   分别写入不同数据后读回，验证不会 false alias。
// ============================================================
static void test_high_bit_alias() {
    TEST_BEGIN("HighBitAlias — bit[53:48] 不同的地址不应 alias");

    printf("[DEBUG] sizeof(Addr_t) = %zu\n", sizeof(Addr_t));
    printf("[DEBUG] sizeof(unsigned long long) = %zu\n", sizeof(unsigned long long));
    printf("[DEBUG] sizeof(size_t) = %zu\n", sizeof(size_t));

    ChiTransactionManager mgr(true);
    const Addr_t addr_lo = 0x000010001000ULL;  // bit[53:48] = 0
    const Addr_t addr_hi = 0x030010001000ULL;  // bit[53:48] = 0x03

    printf("[DEBUG] addr_lo raw: 0x%016" PRIx64 "\n", (uint64_t)addr_lo);
    printf("[DEBUG] addr_hi raw: 0x%016" PRIx64 "\n", (uint64_t)addr_hi);

    Addr_t aligned_lo = align_to_cacheline(addr_lo);
    Addr_t aligned_hi = align_to_cacheline(addr_hi);
    printf("[DEBUG] aligned_lo:  0x%016" PRIx64 "\n", (uint64_t)aligned_lo);
    printf("[DEBUG] aligned_hi:  0x%016" PRIx64 "\n", (uint64_t)aligned_hi);

    ASSERT_NE(aligned_lo, aligned_hi,
              "align_to_cacheline 丢失了高位! (aligned_lo == aligned_hi)");

    printf("[DEBUG] Step 1: Writing 0xAA to addr_lo (0x%016" PRIx64 ")\n", (uint64_t)addr_lo);
    do_write(mgr, 0, 1, addr_lo, 100, 0xAA, 100);

    printf("[DEBUG] Step 2: Writing 0xBB to addr_hi (0x%016" PRIx64 ")\n", (uint64_t)addr_hi);
    do_write(mgr, 0, 2, addr_hi, 101, 0xBB, 110);

    printf("[DEBUG] Step 3: Reading from addr_lo (0x%016" PRIx64 "), expecting 0xAA\n", (uint64_t)addr_lo);
    do_read(mgr, 0, 3, addr_lo, 0xAA, 200);
    
    uint64_t err_cnt1 = mgr.get_checker().get_stats().errors;
    printf("[DEBUG] Errors count after reading addr_lo: %" PRIu64 "\n", err_cnt1);
    ASSERT_EQ(err_cnt1, 0,
              "读 addr_lo 得到错误数据 — 高位被截断导致 alias (可能数据被覆盖为 0xBB)");

    printf("[DEBUG] Step 4: Reading from addr_hi (0x%016" PRIx64 "), expecting 0xBB\n", (uint64_t)addr_hi);
    do_read(mgr, 0, 4, addr_hi, 0xBB, 210);

    uint64_t err_cnt2 = mgr.get_checker().get_stats().errors;
    printf("[DEBUG] Errors count after reading addr_hi: %" PRIu64 "\n", err_cnt2);
    ASSERT_EQ(err_cnt2, 0,
              "读 addr_hi 得到错误数据 — 高位被截断导致 alias");

    ASSERT_EQ(mgr.get_checker().get_stats().passes, 2,
              "两次读应全部 PASS");
    TEST_PASS();
}

// ============================================================
// Test 2: 覆盖检测（最直接）
//   先写 addr_hi (0xFF)，再写 addr_lo (0x00)，读 addr_hi。
//   如果高位被截断，addr_lo 会覆盖 addr_hi → 读到 0x00。
// ============================================================
static void test_overwrite_detection() {
    TEST_BEGIN("OverwriteDetect — 后写低位地址不应覆盖高位地址数据");

    ChiTransactionManager mgr(true);
    const Addr_t addr_hi = 0x010000006000ULL;  // bit[48] = 1
    const Addr_t addr_lo = 0x000000006000ULL;  // bit[48] = 0

    do_write(mgr, 0, 1, addr_hi, 300, 0xFF, 100);
    do_write(mgr, 0, 2, addr_lo, 301, 0x00, 110);

    // 读 addr_hi → 如果没有 alias，应得到 0xFF
    do_read(mgr, 0, 3, addr_hi, 0xFF, 200);

    ASSERT_EQ(mgr.get_checker().get_stats().errors, 0,
              "addr_hi 数据被 addr_lo 覆盖! 确认地址 alias (高位截断)");
    TEST_PASS();
}

// ============================================================
// Test 3: 逐位扫描 bit[48]~bit[53]
//   每一位单独置 1，写入唯一 pattern 后读回。
//   确认每个高位 bit 都被独立保留。
// ============================================================
static void test_bit_scan() {
    TEST_BEGIN("BitScan — bit[48]~bit[53] 逐位独立保留验证");

    ChiTransactionManager mgr(true);
    const Addr_t base = 0x000000002000ULL;

    for (int bit = 48; bit <= 53; ++bit) {
        Addr_t addr = base | (1ULL << bit);
        uint8_t fill = static_cast<uint8_t>(0xA0 + (bit - 48));
        uint32_t idx = bit - 48;
        do_write(mgr, 0, 100 + idx, addr, 300 + idx, fill, 100 + idx);
    }

    uint64_t prev_errors = 0;
    for (int bit = 48; bit <= 53; ++bit) {
        Addr_t addr = base | (1ULL << bit);
        uint8_t expected = static_cast<uint8_t>(0xA0 + (bit - 48));
        uint32_t idx = bit - 48;
        
        printf("[DEBUG] Reading bit %d (addr: 0x%llx)...\n", bit, (unsigned long long)addr);
        do_read(mgr, 0, 200 + idx, addr, expected, 200 + idx);
        
        uint64_t curr_errors = mgr.get_checker().get_stats().errors;
        if (curr_errors > prev_errors) {
            printf("[DEBUG] -> FAIL: Read for bit %d failed! (addr: 0x%llx)\n", bit, (unsigned long long)addr);
            prev_errors = curr_errors;
        } else {
            printf("[DEBUG] -> PASS: Read for bit %d matched.\n", bit);
        }
    }

    auto s = mgr.get_checker().get_stats();
    ASSERT_EQ(s.errors, 0, "某些高位 bit 丢失，导致地址 alias");
    ASSERT_EQ(s.passes, 6, "6 个高位 bit 应各自独立 PASS");
    TEST_PASS();
}

// ============================================================
// Test 4: 截断注入复现
//   用完整 54-bit 地址写入，用截断后 48-bit 地址读回。
//   预期：读到全 0（该截断地址从未写过）→ checker 报错。
//   如果不报错，说明软件侧也有截断行为。
// ============================================================
static void test_truncation_injection() {
    TEST_BEGIN("TruncInject — 模拟硬件截断，验证软件侧正确区分");

    ChiTransactionManager mgr(true);
    const Addr_t full_addr  = 0x0002000000004000ULL;  // bit[49] = 1
    const Addr_t trunc_addr = full_addr & 0x0000FFFFFFFFFFFFULL;

    ASSERT_NE(full_addr, trunc_addr, "测试地址构造错误: 没有高位");

    do_write(mgr, 0, 1, full_addr, 100, 0xDD, 100);

    // 用截断地址读 0xDD → 该地址从未写过，strict mode 下应报错
    do_read(mgr, 0, 2, trunc_addr, 0xDD, 200);

    auto s = mgr.get_checker().get_stats();
    ASSERT_TRUE(s.errors > 0,
                "意外: 截断地址读到了正确数据! 说明软件侧也在截断高位");

    printf("  (预期行为: checker 正确报错 errors=%" PRIu64 ")\n", s.errors);
    TEST_PASS();
}

// ============================================================
// Test 5: 配置文件读取与地址软过滤测试
// ============================================================
static void test_config_and_address_soft_filtering() {
    TEST_BEGIN("ConfigAndSoftFiltering — 配置文件解析与地址过滤不报错验证");

    // 1. 写临时配置文件
    const char* cfg_file = "test_refmodel.cfg";
    FILE* fp = fopen(cfg_file, "w");
    ASSERT_TRUE(fp != nullptr, "无法创建临时配置文件");
    fprintf(fp, "# 测试配置\n");
    fprintf(fp, "strict_mode = 1\n");
    fprintf(fp, "log_level = 3\n");
    fprintf(fp, "addr_filter_mask = 0x0030000000000000\n");
    fprintf(fp, "enable_soft_ctrl = 0\n");
    fclose(fp);

    // 2. 验证配置文件解析
    RefModelConfig cfg = load_config(cfg_file);
    remove(cfg_file); // 用完立即删除

    ASSERT_EQ(cfg.loaded_from_file, true, "配置文件加载失败");
    ASSERT_EQ(cfg.strict_mode, 1, "strict_mode 解析错误");
    ASSERT_EQ(cfg.log_level, 3, "log_level 解析错误");
    ASSERT_EQ(cfg.addr_filter_mask, 0x0030000000000000ULL, "addr_filter_mask 解析错误");
    ASSERT_EQ(cfg.enable_soft_ctrl, false, "enable_soft_ctrl 解析错误");

    // 3. 验证地址软过滤行为
    // mask = 0x0030000000000000ULL
    ChiTransactionManager mgr(true, cfg.addr_filter_mask);

    Addr_t normal_addr   = 0x000010001000ULL; // (normal_addr & mask) == 0 -> 正常
    Addr_t filtered_addr = 0x0010000000002000ULL; // (filtered_addr & mask) != 0 -> 过滤

    // 验证 is_addr_filtered() 方法
    ASSERT_EQ(mgr.is_addr_filtered(normal_addr), false, "正常地址被错误过滤");
    ASSERT_EQ(mgr.is_addr_filtered(filtered_addr), true, "过滤地址未生效");

    // A. 正常地址读写不一致 -> 报错
    do_write(mgr, 0, 1, normal_addr, 100, 0xAA, 100);
    do_read(mgr, 0, 2, normal_addr, 0xBB, 200); // 故意读错值 (预期BB，实际AA)
    
    auto s1 = mgr.get_checker().get_stats();
    ASSERT_EQ(s1.errors, 1, "正常地址数据不匹配应该报错 errors=1");
    ASSERT_EQ(s1.filtered_mismatches, 0, "正常地址不应计入 filtered_mismatches");

    // B. 被过滤的地址读写不一致 -> 不报错，计入 filtered_mismatches
    do_write(mgr, 0, 3, filtered_addr, 101, 0xAA, 300);
    do_read(mgr, 0, 4, filtered_addr, 0xBB, 400); // 故意读错值 (预期BB，实际AA)

    auto s2 = mgr.get_checker().get_stats();
    ASSERT_EQ(s2.errors, 1, "过滤地址数据不匹配时不应该增加 errors");
    ASSERT_EQ(s2.filtered_mismatches, 1, "过滤地址数据不匹配应该增加 filtered_mismatches=1");

    TEST_PASS();
}

// ============================================================
// main
// ============================================================
int main() {
    printf("========================================\n");
    printf(" 54-bit 地址截断诊断测试 (无 GTest)\n");
    printf("========================================\n");

    test_high_bit_alias();
    test_overwrite_detection();
    test_bit_scan();
    test_truncation_injection();
    test_config_and_address_soft_filtering();

    printf("\n========================================\n");
    printf(" 结果: %d/%d PASS, %d FAIL\n", g_pass, g_total, g_fail);
    printf("========================================\n");

    if (g_fail == 0) {
        printf("\n✓ 所有测试 PASS → C++ 软件侧 54-bit 地址处理正确\n");
        printf("  → 问题在硬件/SV 侧 (tracker / flit 域提取)\n\n");
    } else {
        printf("\n✗ 存在 FAIL → C++ 软件侧有地址截断缺陷!\n");
        printf("  → 检查 Addr_t 类型 / align_to_cacheline / DPI 转换链\n\n");
    }

    return g_fail > 0 ? 1 : 0;
}
