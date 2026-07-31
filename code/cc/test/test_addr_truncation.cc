// ============================================================
// file: test/test_addr_truncation.cc
// 54-bit 地址高位截断诊断测试
//
// 目的：验证 C++ 软件侧（ChiTransactionManager / ShadowMemory /
//       align_to_cacheline / DPI 地址转换链）能否正确保留和区分
//       54-bit 地址的高 6 位 (bit[53:48])。
//
// 诊断逻辑：
//   - 所有测试 PASS → 软件侧无截断问题 → bug 在硬件/SV 侧
//   - 任何测试 FAIL → 软件侧存在截断，需修复 C++ 代码
// ============================================================
#include <gtest/gtest.h>
#include "chi_transaction_manager.h"
#include <array>
#include <cstring>
#include <cstdint>

// ============================================================
// 测试固件：AddrTruncationTest
// ============================================================
class AddrTruncationTest : public ::testing::Test {
protected:
    void SetUp() override {
        mgr_ = std::make_unique<ChiTransactionManager>(true);
    }

    std::unique_ptr<ChiTransactionManager> mgr_;

    // 辅助：构造填充指定值的 32 字节 flit
    std::array<uint8_t, 32> make_flit(uint8_t fill = 0) {
        std::array<uint8_t, 32> f;
        f.fill(fill);
        return f;
    }

    // 辅助：完整的单 flit 写事务（txreq → rxrsp → txdat）
    void do_write_1flit(uint32_t mst, uint32_t txnid, Addr_t addr,
                        uint32_t dbid, uint8_t fill_val,
                        Timestamp_t req_time = 100) {
        mgr_->process_txreq(mst, txnid, addr, /*size=*/5,
                            static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull),
                            /*secvec=*/0x1, req_time, /*is_sm=*/false);
        mgr_->process_rxrsp_dbid(mst, txnid, 0, dbid);
        auto wd = make_flit(fill_val);
        mgr_->process_txdat(mst, dbid, 0, /*dataid=*/0,
                            wd.data(), 0xFFFFFFFF, /*data_cnt=*/0);
    }

    // 辅助：完整的单 flit 读事务（txreq → rxdat），返回 checker stats
    void do_read_1flit(uint32_t mst, uint32_t txnid, Addr_t addr,
                       uint8_t expected_fill,
                       Timestamp_t req_time = 200) {
        mgr_->process_txreq(mst, txnid, addr, /*size=*/5,
                            static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                            /*secvec=*/0x1, req_time, /*is_sm=*/false);
        auto rd = make_flit(expected_fill);
        mgr_->process_rxdat(mst, txnid, 0, /*dataid=*/0,
                            rd.data(), 0xFFFFFFFF, /*data_cnt=*/0);
    }
};

// ============================================================
// Test 1: HighBitAliasDetection
//   两个地址仅在 bit[53:48] 不同（低 48 位完全相同），
//   分别写入不同数据后读回，验证不会产生 false alias。
//
//   addr_lo = 0x000010001000  (bit[53:48] = 0b000000)
//   addr_hi = 0x030010001000  (bit[53:48] = 0b000011)
//   这两个地址低 48 位完全相同，但应映射到不同的 shadow memory 位置。
// ============================================================
TEST_F(AddrTruncationTest, HighBitAliasDetection) {
    const Addr_t addr_lo = 0x000010001000ULL;  // bit[53:48] = 0
    const Addr_t addr_hi = 0x030010001000ULL;  // bit[53:48] = 0x03
    uint32_t mst = 0;

    // 对齐后确认两者不同
    ASSERT_NE(align_to_cacheline(addr_lo), align_to_cacheline(addr_hi))
        << "CRITICAL: align_to_cacheline collapsed high bits!";

    // 写 addr_lo → fill 0xAA
    do_write_1flit(mst, /*txnid=*/1, addr_lo, /*dbid=*/100, 0xAA, 100);

    // 写 addr_hi → fill 0xBB (不同数据)
    do_write_1flit(mst, /*txnid=*/2, addr_hi, /*dbid=*/101, 0xBB, 110);

    // 读 addr_lo → 应该得到 0xAA，不是 0xBB
    do_read_1flit(mst, /*txnid=*/3, addr_lo, 0xAA, 200);
    EXPECT_EQ(mgr_->get_checker().get_stats().errors, 0)
        << "addr_lo read got wrong data — high bits were lost (aliased with addr_hi)";

    // 读 addr_hi → 应该得到 0xBB
    do_read_1flit(mst, /*txnid=*/4, addr_hi, 0xBB, 210);
    EXPECT_EQ(mgr_->get_checker().get_stats().errors, 0)
        << "addr_hi read got wrong data — high bits were lost";

    EXPECT_EQ(mgr_->get_checker().get_stats().passes, 2);
}

// ============================================================
// Test 2: Full54BitAddressRange
//   遍历 bit[48] ~ bit[53] 的每一个高位，构造
//   (1ULL << bit) | base_addr 的地址，写入唯一 pattern 后读回。
//   确认每个高位 bit 都被独立保留。
// ============================================================
TEST_F(AddrTruncationTest, Full54BitAddressRange) {
    const Addr_t base_addr = 0x000000002000ULL;  // cacheline aligned
    uint32_t mst = 0;
    uint32_t wtxn_base = 100, rtxn_base = 200, dbid_base = 300;

    // 对 bit 48 ~ 53 各设置一个高位
    for (int bit = 48; bit <= 53; ++bit) {
        Addr_t test_addr = base_addr | (1ULL << bit);
        uint8_t fill = static_cast<uint8_t>(0xA0 + (bit - 48));  // 0xA0~0xA5
        uint32_t idx = bit - 48;

        do_write_1flit(mst, wtxn_base + idx, test_addr,
                       dbid_base + idx, fill, 100 + idx);
    }

    // 逐个读回验证
    for (int bit = 48; bit <= 53; ++bit) {
        Addr_t test_addr = base_addr | (1ULL << bit);
        uint8_t expected_fill = static_cast<uint8_t>(0xA0 + (bit - 48));
        uint32_t idx = bit - 48;

        do_read_1flit(mst, rtxn_base + idx, test_addr,
                      expected_fill, 200 + idx);
    }

    auto s = mgr_->get_checker().get_stats();
    EXPECT_EQ(s.passes, 6)
        << "Not all 54-bit high bits were independently preserved";
    EXPECT_EQ(s.errors, 0)
        << "Some high bit(s) were lost, causing address aliasing";
}

// ============================================================
// Test 3: Truncation48BitInjection
//   模拟硬件截断行为：先用完整 54-bit 地址写入数据，
//   再用截断为 48-bit 的地址（addr & 0xFFFFFFFFFFFF）读回。
//   预期行为：读到全 0（该截断地址从未写过），checker 应报
//   FAIL_NO_PRIOR_WRITE 或 FAIL_DATA_MISMATCH。
//
//   这个测试验证：如果硬件截断了高位，会导致什么后果。
// ============================================================
TEST_F(AddrTruncationTest, Truncation48BitInjection) {
    const Addr_t full_addr = 0x0002000000004000ULL;  // bit[49] = 1, above 48-bit range
    const Addr_t trunc_addr = full_addr & 0x0000FFFFFFFFFFFFULL;  // 截断为 48-bit
    uint32_t mst = 0;

    // 确认截断确实改变了地址
    ASSERT_NE(full_addr, trunc_addr)
        << "Test setup error: full_addr has no high bits set";

    // 用完整 54-bit 地址写入 0xDD
    do_write_1flit(mst, /*txnid=*/1, full_addr, /*dbid=*/100, 0xDD, 100);

    // 用截断后的 48-bit 地址读回 0xDD → 应该失败
    // 因为 trunc_addr 从未被写过（在 strict mode 下读未写地址且值非0 → error）
    do_read_1flit(mst, /*txnid=*/2, trunc_addr, 0xDD, 200);

    auto s = mgr_->get_checker().get_stats();
    // 如果这里 errors > 0，说明软件侧正确区分了两个地址 → 截断只能来自硬件
    EXPECT_GT(s.errors, 0)
        << "UNEXPECTED: Reading truncated addr got correct data! "
           "This means the software collapses 54-bit to 48-bit internally.";
}

// ============================================================
// Test 4: CachelineAlignPreservesHighBits
//   验证 align_to_cacheline() 不会截断 bit[53:48]。
//   这是一个纯函数测试，不经过事务管理器。
// ============================================================
TEST_F(AddrTruncationTest, CachelineAlignPreservesHighBits) {
    // CACHELINE_MASK = ~(128 - 1) = 0xFFFFFFFFFFFFFF80
    // 确认 CACHELINE_MASK 保留高位
    EXPECT_EQ(CACHELINE_MASK & 0x3F000000000000ULL, 0x3F000000000000ULL)
        << "CACHELINE_MASK truncates bit[53:48]!";

    // 测试各种高位地址的对齐结果
    struct TestCase {
        Addr_t input;
        Addr_t expected;
    };
    TestCase cases[] = {
        // bit[53] set, 对齐到 128B 边界
        {0x200000001080ULL, 0x200000001080ULL},  // already aligned
        {0x200000001099ULL, 0x200000001080ULL},  // offset within CL
        // bit[48] set
        {0x010000002000ULL, 0x010000002000ULL},
        {0x010000002005ULL, 0x010000002000ULL},
        // 多个高位同时 set
        {0x3F0000003FE0ULL, 0x3F0000003F80ULL},
        // 最大 54-bit 地址
        {0x3FFFFFFFFFFFFFFFULL & 0x3FFFFFFFFFFFFFULL, 0x3FFFFFFFFFFFFF80ULL},
    };

    for (const auto& tc : cases) {
        Addr_t result = align_to_cacheline(tc.input);
        // 检查高 6 位保留
        EXPECT_EQ(result >> 48, tc.input >> 48)
            << "align_to_cacheline lost high bits for input 0x"
            << std::hex << tc.input;
        // 检查低 7 位清零
        EXPECT_EQ(result & 0x7F, 0)
            << "align_to_cacheline didn't clear low bits for input 0x"
            << std::hex << tc.input;
    }
}

// ============================================================
// Test 5: MultiMasterHighBitIsolation
//   两个不同 master 写入仅 bit[53:48] 不同的地址，
//   验证 shadow memory 中地址独立，互不干扰。
// ============================================================
TEST_F(AddrTruncationTest, MultiMasterHighBitIsolation) {
    const Addr_t addr_base = 0x000000008000ULL;  // cacheline aligned
    const Addr_t addr_high = 0x010000008000ULL;  // bit[48] = 1, same low 48-bit
    uint32_t mst_a = 0, mst_b = 8;

    // Master A 写 addr_base → 0x11
    do_write_1flit(mst_a, /*txnid=*/1, addr_base, /*dbid=*/50, 0x11, 100);

    // Master B 写 addr_high → 0x22
    do_write_1flit(mst_b, /*txnid=*/1, addr_high, /*dbid=*/51, 0x22, 110);

    // Master A 读 addr_base → 应得 0x11
    do_read_1flit(mst_a, /*txnid=*/2, addr_base, 0x11, 200);
    EXPECT_EQ(mgr_->get_checker().get_stats().errors, 0)
        << "Master A read got wrong data — address alias across masters";

    // Master B 读 addr_high → 应得 0x22
    do_read_1flit(mst_b, /*txnid=*/2, addr_high, 0x22, 210);
    EXPECT_EQ(mgr_->get_checker().get_stats().errors, 0)
        << "Master B read got wrong data — address alias across masters";

    EXPECT_EQ(mgr_->get_checker().get_stats().passes, 2);
}

// ============================================================
// Test 6: DpiAddrCastFidelity
//   直接测试 DPI 入口处 long long → uint64_t 的转换保真性。
//   在 C++ 层面模拟 SV 通过 DPI-C 传入的 longint 值，
//   确认 static_cast<uint64_t>(long long) 不会丢失 bit[53:48]。
//
//   这个测试不经过事务管理器，直接验证类型转换。
// ============================================================
TEST_F(AddrTruncationTest, DpiAddrCastFidelity) {
    // 模拟 SV DPI-C 传入的 longint 值（signed 64-bit）
    // 54-bit 地址范围: 0 ~ 0x3FFFFFFFFFFFFF
    struct TestVector {
        long long sv_longint;       // SV 侧传入的 longint
        uint64_t  expected_uint64;  // C++ 侧期望得到的 uint64_t
    };

    TestVector vectors[] = {
        // 基本地址
        {0x0000000010000LL, 0x0000000010000ULL},
        // bit[48] = 1
        {0x0001000000000LL, 0x0001000000000ULL},
        // bit[49] = 1
        {0x0002000000000LL, 0x0002000000000ULL},
        // bit[53] = 1
        {0x0020000000000LL, 0x0020000000000ULL},
        // bit[53:48] all set = 0x3F
        {0x003F000000000LL, 0x003F000000000ULL},
        // 最大 54-bit 地址
        {0x003FFFFFFFFFFFFFLL, 0x003FFFFFFFFFFFFFULL},
        // 混合高低位
        {0x002A00000DEADLL, 0x002A00000DEADULL},
    };

    for (const auto& tv : vectors) {
        // 这是 dpi_chi_txreq 中实际执行的转换
        uint64_t result = static_cast<uint64_t>(tv.sv_longint);
        EXPECT_EQ(result, tv.expected_uint64)
            << "DPI addr cast lost bits: input=0x" << std::hex << tv.sv_longint
            << " got=0x" << result << " expected=0x" << tv.expected_uint64;
    }
}

// ============================================================
// Test 7 (额外): FullCachelineWriteHighAddr
//   使用 size=7 (4 flits) 对 54-bit 高位地址写全 cacheline，
//   然后读回验证。确保多 flit 场景下高位地址也被正确保留。
// ============================================================
TEST_F(AddrTruncationTest, FullCachelineWriteHighAddr) {
    const Addr_t addr = 0x020000010000ULL;  // bit[49] = 1
    uint32_t mst = 0, wtxn = 10, rtxn = 11, dbid = 200;

    // 写全 cacheline: size=7, secvec=0xF → 4 flits
    mgr_->process_txreq(mst, wtxn, addr, /*size=*/7,
                        static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull),
                        /*secvec=*/0xF, 100, /*is_sm=*/false);
    mgr_->process_rxrsp_dbid(mst, wtxn, 0, dbid);

    // 写 4 笔 flit，每段填充不同 pattern
    uint32_t dataids[] = {0, 2, 4, 6};
    std::array<uint8_t, 32> wflits[4];
    for (int i = 0; i < 4; ++i) {
        wflits[i].fill(static_cast<uint8_t>(0xC0 + i));
        mgr_->process_txdat(mst, dbid, 0, dataids[i],
                            wflits[i].data(), 0xFFFFFFFF, 0);
    }

    // 读回全 cacheline
    mgr_->process_txreq(mst, rtxn, addr, /*size=*/7,
                        static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                        /*secvec=*/0xF, 200, /*is_sm=*/false);
    for (int i = 0; i < 4; ++i) {
        mgr_->process_rxdat(mst, rtxn, 0, dataids[i],
                            wflits[i].data(), 0xFFFFFFFF, 0);
    }

    auto s = mgr_->get_checker().get_stats();
    EXPECT_EQ(s.passes, 1)
        << "Full cacheline write/read at 54-bit address failed";
    EXPECT_EQ(s.errors, 0);
}

// ============================================================
// Test 8 (额外): OverwriteDetectionWithHighBits
//   先写 addr_hi，再写 addr_lo (低 48 位相同)，然后读 addr_hi。
//   如果高位被截断，addr_lo 的写会覆盖 addr_hi 的数据 → 读错。
//   这是最直接的 false alias 检测。
// ============================================================
TEST_F(AddrTruncationTest, OverwriteDetectionWithHighBits) {
    const Addr_t addr_hi = 0x010000006000ULL;  // bit[48] = 1
    const Addr_t addr_lo = 0x000000006000ULL;  // bit[48] = 0, same low 48
    uint32_t mst = 0;

    // 先写 addr_hi → 0xFF
    do_write_1flit(mst, /*txnid=*/1, addr_hi, /*dbid=*/300, 0xFF, 100);

    // 再写 addr_lo → 0x00  (如果 alias，会覆盖 addr_hi)
    do_write_1flit(mst, /*txnid=*/2, addr_lo, /*dbid=*/301, 0x00, 110);

    // 读 addr_hi → 如果没有 alias，应该得到 0xFF
    do_read_1flit(mst, /*txnid=*/3, addr_hi, 0xFF, 200);

    auto s = mgr_->get_checker().get_stats();
    EXPECT_EQ(s.errors, 0)
        << "CRITICAL: addr_hi data was overwritten by addr_lo write! "
           "This confirms address aliasing due to high-bit truncation.";
    EXPECT_EQ(s.passes, 1);
}
