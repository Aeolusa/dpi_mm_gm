// ============================================================
// file: test/integration_test.cc
// CHI Transaction Manager 集成测试
//   - 适配新 API: process_txreq +is_sm, size-based flit count
//   - 覆盖 corner case: 多段写、SM data_cnt、多 dataid 组装、
//     多 master 交叉、dataid 编码验证
// ============================================================
#include <gtest/gtest.h>
#include "chi_transaction_manager.h"
#include "logger.h"
#include <array>
#include <cstring>

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::instance().set_level(LogLevel::DEBUG);
        // strict mode
        mgr_ = std::make_unique<ChiTransactionManager>(true);
    }

    std::unique_ptr<ChiTransactionManager> mgr_;

    // 辅助：构造全 0 的 32 字节 flit
    std::array<uint8_t, 32> make_flit(uint8_t fill = 0) {
        std::array<uint8_t, 32> f;
        f.fill(fill);
        return f;
    }

    // 辅助：构造带前 4 字节 pattern 的 flit
    std::array<uint8_t, 32> make_flit(uint8_t b0, uint8_t b1,
                                       uint8_t b2, uint8_t b3) {
        auto f = make_flit(uint8_t(0));
        f[0] = b0; f[1] = b1; f[2] = b2; f[3] = b3;
        return f;
    }
};

// ============================================================
// 基本写读流程（非 SM，size=5 → 1 flit）
// ============================================================
TEST_F(IntegrationTest, NormalWriteReadFlow) {
    uint32_t mst = 1, wtxn = 10, rtxn = 11, dbid = 50;
    Addr_t addr = 0x1000;

    // secvec=0x1 (bit0), size=5 → 1 flit, dataid=0
    mgr_->process_txreq(mst, wtxn, addr, 5,
                        static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull),
                        0x1, 100, /*is_sm=*/false);
    mgr_->process_rxrsp_dbid(mst, wtxn, 0, dbid, /*srcid=*/0);

    auto wd = make_flit(0xDE, 0xAD, 0xBE, 0xEF);
    mgr_->process_txdat(mst, dbid, 0, /*dataid=*/0, wd.data(), 0xF, /*data_cnt=*/0, /*tgtid=*/0);

    // Read back
    mgr_->process_txreq(mst, rtxn, addr, 5,
                        static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                        0x1, 200, false);
    mgr_->process_rxdat(mst, rtxn, 0, 0, wd.data(), 0xF, 0);

    auto s = mgr_->get_checker().get_stats();
    EXPECT_EQ(s.passes, 1);
    EXPECT_EQ(s.errors, 0);
}

// ============================================================
// 读未写地址 → error
// ============================================================
TEST_F(IntegrationTest, ReadErrorFlow) {
    uint32_t mst = 1, rtxn = 12;
    Addr_t addr = 0x2000;

    auto rd = make_flit(0x99, 0x99, 0, 0);
    mgr_->process_txreq(mst, rtxn, addr, 5,
                        static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                        0x1, 100, false);
    mgr_->process_rxdat(mst, rtxn, 0, 0, rd.data(), 0x3, 0);

    EXPECT_EQ(mgr_->get_checker().get_stats().errors, 1);
}

// ============================================================
// 部分写读（WriteNoSnpPtl，byte enable 部分使能）
// ============================================================
TEST_F(IntegrationTest, PartialWriteReadFlow) {
    uint32_t mst = 1, wtxn = 20, rtxn = 21, dbid = 60;
    Addr_t addr = 0x3000;

    mgr_->process_txreq(mst, wtxn, addr, 5,
                        static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpPtl),
                        0x1, 100, false);
    mgr_->process_rxrsp_dbid(mst, wtxn, 0, dbid, /*srcid=*/0);

    auto wd = make_flit(0xAA, 0xBB, 0xCC, 0xDD);
    // Only enable byte 0 and 2
    mgr_->process_txdat(mst, dbid, 0, 0, wd.data(), 0x5, 0, /*tgtid=*/0);

    // Read: expect 0xAA at byte0, 0x00 at byte1, 0xCC at byte2
    mgr_->process_txreq(mst, rtxn, addr, 5,
                        static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                        0x1, 200, false);
    auto rd = make_flit(0xAA, 0x00, 0xCC, 0x00);
    mgr_->process_rxdat(mst, rtxn, 0, 0, rd.data(), 0xF, 0);

    auto s = mgr_->get_checker().get_stats();
    EXPECT_EQ(s.passes, 1);
    EXPECT_EQ(s.errors, 0);
}

// ============================================================
// 压力测试：100 笔写 + 100 笔读校验
// ============================================================
TEST_F(IntegrationTest, StressTestFlow) {
    const int N = 100;
    uint32_t mst = 2;

    for (int i = 0; i < N; ++i) {
        uint32_t txnid = 100 + i, dbid = 1000 + i;
        Addr_t addr = 0x4000 + i * 128;

        mgr_->process_txreq(mst, txnid, addr, 5,
                            static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull),
                            0x1, 100 + i, false);
        mgr_->process_rxrsp_dbid(mst, txnid, 0, dbid, /*srcid=*/0);

        auto wd = make_flit(0);
        wd[0] = static_cast<uint8_t>(i & 0xFF);
        mgr_->process_txdat(mst, dbid, 0, 0, wd.data(), 0x1, 0, /*tgtid=*/0);
    }

    for (int i = 0; i < N; ++i) {
        uint32_t txnid = 200 + i;
        Addr_t addr = 0x4000 + i * 128;

        mgr_->process_txreq(mst, txnid, addr, 5,
                            static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                            0x1, 1000 + i, false);
        auto rd = make_flit(0);
        rd[0] = static_cast<uint8_t>(i & 0xFF);
        mgr_->process_rxdat(mst, txnid, 0, 0, rd.data(), 0x1, 0);
    }

    auto s = mgr_->get_checker().get_stats();
    EXPECT_EQ(s.passes, N);
    EXPECT_EQ(s.errors, 0);
}

// ============================================================
// size=6 → 2 flits（dataid=0 + dataid=2）
// ============================================================
TEST_F(IntegrationTest, Size6TwoFlits) {
    uint32_t mst = 0, wtxn = 30, rtxn = 31, dbid = 70;
    Addr_t addr = 0x5000;

    // secvec=0x3 (bit0+bit1), size=6 → 2 flits
    mgr_->process_txreq(mst, wtxn, addr, 6,
                        static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull),
                        0x3, 100, false);
    mgr_->process_rxrsp_dbid(mst, wtxn, 0, dbid, /*srcid=*/0);

    auto flit0 = make_flit(0xAA);  // dataid=0 → offset 0
    auto flit1 = make_flit(0xBB);  // dataid=2 → offset 32
    mgr_->process_txdat(mst, dbid, 0, /*dataid=*/0, flit0.data(), 0xFFFFFFFF, 0, /*tgtid=*/0);
    mgr_->process_txdat(mst, dbid, 0, /*dataid=*/2, flit1.data(), 0xFFFFFFFF, 0, /*tgtid=*/0);

    // Read back with 2 flits
    mgr_->process_txreq(mst, rtxn, addr, 6,
                        static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                        0x3, 200, false);
    mgr_->process_rxdat(mst, rtxn, 0, 0, flit0.data(), 0xFFFFFFFF, 0);
    mgr_->process_rxdat(mst, rtxn, 0, 2, flit1.data(), 0xFFFFFFFF, 0);

    auto s = mgr_->get_checker().get_stats();
    EXPECT_EQ(s.passes, 1);
    EXPECT_EQ(s.errors, 0);
}

// ============================================================
// size=7 → 4 flits（dataid=0,2,4,6）全 cacheline 写读
// ============================================================
TEST_F(IntegrationTest, Size7FourFlitsFullCacheline) {
    uint32_t mst = 0, wtxn = 40, rtxn = 41, dbid = 80;
    Addr_t addr = 0x6000;

    // secvec=0xF, size=7 → 4 flits
    mgr_->process_txreq(mst, wtxn, addr, 7,
                        static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull),
                        0xF, 100, false);
    mgr_->process_rxrsp_dbid(mst, wtxn, 0, dbid, /*srcid=*/0);

    // 写 4 笔 flit，每笔填充不同 pattern
    std::array<uint8_t, 32> flits[4];
    uint32_t dataids[] = {0, 2, 4, 6};
    for (int i = 0; i < 4; ++i) {
        flits[i].fill(static_cast<uint8_t>(0x10 * (i + 1)));
        mgr_->process_txdat(mst, dbid, 0, dataids[i],
                            flits[i].data(), 0xFFFFFFFF, 0, /*tgtid=*/0);
    }

    // 读回 4 笔 flit
    mgr_->process_txreq(mst, rtxn, addr, 7,
                        static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                        0xF, 200, false);
    for (int i = 0; i < 4; ++i) {
        mgr_->process_rxdat(mst, rtxn, 0, dataids[i],
                            flits[i].data(), 0xFFFFFFFF, 0);
    }

    auto s = mgr_->get_checker().get_stats();
    EXPECT_EQ(s.passes, 1);
    EXPECT_EQ(s.errors, 0);
}

// ============================================================
// SM 写事务：is_sm=true, data_cnt 决定 flit 数
// ============================================================
TEST_F(IntegrationTest, SmWriteWithDataCnt) {
    uint32_t mst = 0, wtxn = 50, rtxn = 51, dbid = 90;
    Addr_t addr = 0x7000;

    // SM 写，secvec=0x5 (bit0+bit2), 但实际由 data_cnt 决定
    mgr_->process_txreq(mst, wtxn, addr, 7,
                        static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull),
                        0x5, 100, /*is_sm=*/true);
    mgr_->process_rxrsp_dbid(mst, wtxn, 0, dbid, /*srcid=*/0);

    // 首笔 txdat 的 data_cnt=3 → 覆盖 expected_flits 为 3
    auto flit0 = make_flit(0xAA);
    auto flit1 = make_flit(0xBB);
    auto flit2 = make_flit(0xCC);
    mgr_->process_txdat(mst, dbid, 0, /*dataid=*/0, flit0.data(), 0xFFFFFFFF,
                        /*data_cnt=*/3, /*tgtid=*/0);
    mgr_->process_txdat(mst, dbid, 0, /*dataid=*/2, flit1.data(), 0xFFFFFFFF,
                        /*data_cnt=*/3, /*tgtid=*/0);
    mgr_->process_txdat(mst, dbid, 0, /*dataid=*/4, flit2.data(), 0xFFFFFFFF,
                        /*data_cnt=*/3, /*tgtid=*/0);

    // 读回验证（读用 size=7 → 4 flits，但只验证写入的 3 段）
    mgr_->process_txreq(mst, rtxn, addr, 7,
                        static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                        0xF, 200, true);

    auto rd_zero = make_flit(uint8_t(0));
    mgr_->process_rxdat(mst, rtxn, 0, 0, flit0.data(), 0xFFFFFFFF, 0);
    mgr_->process_rxdat(mst, rtxn, 0, 2, flit1.data(), 0xFFFFFFFF, 0);
    mgr_->process_rxdat(mst, rtxn, 0, 4, flit2.data(), 0xFFFFFFFF, 0);
    mgr_->process_rxdat(mst, rtxn, 0, 6, rd_zero.data(), 0xFFFFFFFF, 0);

    auto s = mgr_->get_checker().get_stats();
    EXPECT_EQ(s.passes, 1);
    EXPECT_EQ(s.errors, 0);
}

// ============================================================
// SM 写 data_cnt=1：单笔 SM 写
// ============================================================
TEST_F(IntegrationTest, SmWriteSingleFlit) {
    uint32_t mst = 3, wtxn = 60, rtxn = 61, dbid = 100;
    Addr_t addr = 0x8000;

    // secvec=0x1, is_sm=true, data_cnt 将为 1
    mgr_->process_txreq(mst, wtxn, addr, 5,
                        static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull),
                        0x1, 100, true);
    mgr_->process_rxrsp_dbid(mst, wtxn, 0, dbid, /*srcid=*/0);

    auto flit0 = make_flit(0x55);
    mgr_->process_txdat(mst, dbid, 0, 0, flit0.data(), 0xFFFFFFFF, /*data_cnt=*/1, /*tgtid=*/0);

    // Read verify
    mgr_->process_txreq(mst, rtxn, addr, 5,
                        static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                        0x1, 200, true);
    mgr_->process_rxdat(mst, rtxn, 0, 0, flit0.data(), 0xFFFFFFFF, 0);

    auto s = mgr_->get_checker().get_stats();
    EXPECT_EQ(s.passes, 1);
    EXPECT_EQ(s.errors, 0);
}

// ============================================================
// CHI TxnID 复用场景测试（对应 Bug 现场）
//   t0: txreq addr=a, txnid=89 (is_sm=false)
//   t1: rxrsp CompDBIDResp txnid=89, dbid=3, srcid=0xd4
//   t2: txreq addr=b, txnid=89
//   t3: rxrsp CompDBIDResp txnid=89, dbid=1, srcid=0xf3
//   t4: txdat txnid=3 (dbid), tgtid=0xd4 -> 对应 addr_a
//   t5: txdat txnid=1 (dbid), tgtid=0xf3 -> 对应 addr_b
// ============================================================
TEST_F(IntegrationTest, TxnIdReuseFlow) {
    uint32_t mst = 0, txnid = 89;
    Addr_t addr_a = 0x10000;
    Addr_t addr_b = 0x20000;

    // t0: Write NoSnp to addr_a with txnid=89
    mgr_->process_txreq(mst, txnid, addr_a, 5,
                        static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull),
                        0x1, 100, false);

    // t1: CompDBIDResp for addr_a, dbid=3, srcid=0xd4 (CompDBIDResp opcode = 5)
    mgr_->process_rxrsp_dbid(mst, txnid, 5, 3, 0xd4);

    // t2: Write NoSnp to addr_b with txnid=89 (TxnID reuse!)
    mgr_->process_txreq(mst, txnid, addr_b, 5,
                        static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull),
                        0x1, 110, false);

    // t3: CompDBIDResp for addr_b, dbid=1, srcid=0xf3
    mgr_->process_rxrsp_dbid(mst, txnid, 5, 1, 0xf3);

    // t4: txdat for dbid=3, tgtid=0xd4 (addr_a)
    auto flit_a = make_flit(0xAA);
    mgr_->process_txdat(mst, 3, 0, 0, flit_a.data(), 0xFFFFFFFF, 0, 0xd4);

    // t5: txdat for dbid=1, tgtid=0xf3 (addr_b)
    auto flit_b = make_flit(0xBB);
    mgr_->process_txdat(mst, 1, 0, 0, flit_b.data(), 0xFFFFFFFF, 0, 0xf3);

    // Verify checker states
    auto s = mgr_->get_checker().get_stats();
    EXPECT_EQ(s.errors, 0);

    // Read back addr_a
    mgr_->process_txreq(mst, 90, addr_a, 5,
                        static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                        0x1, 200, false);
    mgr_->process_rxdat(mst, 90, 0, 0, flit_a.data(), 0xFFFFFFFF, 0);

    // Read back addr_b
    mgr_->process_txreq(mst, 91, addr_b, 5,
                        static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                        0x1, 210, false);
    mgr_->process_rxdat(mst, 91, 0, 0, flit_b.data(), 0xFFFFFFFF, 0);

    s = mgr_->get_checker().get_stats();
    EXPECT_EQ(s.passes, 2);
    EXPECT_EQ(s.errors, 0);
}

// ============================================================
// dataid 编码验证：确认 dataid → offset 映射正确
//   dataid=0→offset 0, dataid=2→32, dataid=4→64, dataid=6→96
// ============================================================
TEST_F(IntegrationTest, DataidOffsetMapping) {
    uint32_t mst = 0, wtxn = 70, rtxn = 71, dbid = 110;
    Addr_t addr = 0x9000;

    // 写全 cacheline (size=7, secvec=0xF)
    mgr_->process_txreq(mst, wtxn, addr, 7,
                        static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull),
                        0xF, 100, false);
    mgr_->process_rxrsp_dbid(mst, wtxn, 0, dbid, /*srcid=*/0);

    // 每段 32B 填充 dataid 值本身作为 pattern
    uint32_t ids[] = {0, 2, 4, 6};
    std::array<uint8_t, 32> wflits[4];
    for (int i = 0; i < 4; ++i) {
        wflits[i].fill(static_cast<uint8_t>(ids[i]));
        mgr_->process_txdat(mst, dbid, 0, ids[i],
                            wflits[i].data(), 0xFFFFFFFF, 0, /*tgtid=*/0);
    }

    // 读回：故意用乱序 dataid 到达
    mgr_->process_txreq(mst, rtxn, addr, 7,
                        static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                        0xF, 200, false);
    // 先收 dataid=6, 再 4, 2, 0（乱序验证组装逻辑）
    mgr_->process_rxdat(mst, rtxn, 0, 6, wflits[3].data(), 0xFFFFFFFF, 0);
    mgr_->process_rxdat(mst, rtxn, 0, 4, wflits[2].data(), 0xFFFFFFFF, 0);
    mgr_->process_rxdat(mst, rtxn, 0, 2, wflits[1].data(), 0xFFFFFFFF, 0);
    mgr_->process_rxdat(mst, rtxn, 0, 0, wflits[0].data(), 0xFFFFFFFF, 0);

    auto s = mgr_->get_checker().get_stats();
    EXPECT_EQ(s.passes, 1);
    EXPECT_EQ(s.errors, 0);
}

// ============================================================
// 非连续 secvec 的 SM 写：secvec=0xA (bit1+bit3) → dataid=2,6
// ============================================================
TEST_F(IntegrationTest, SmNonContiguousSecvec) {
    uint32_t mst = 4, wtxn = 80, rtxn = 81, dbid = 120;
    Addr_t addr = 0xA000;

    // secvec=0xA (bit1+bit3), SM write, popcount=2
    // data_cnt 将覆盖为 2
    mgr_->process_txreq(mst, wtxn, addr, 7,
                        static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull),
                        0xA, 100, true);
    mgr_->process_rxrsp_dbid(mst, wtxn, 0, dbid, /*srcid=*/0);

    auto flit_seg1 = make_flit(0x11);  // dataid=2 → offset 32
    auto flit_seg3 = make_flit(0x33);  // dataid=6 → offset 96
    mgr_->process_txdat(mst, dbid, 0, 2, flit_seg1.data(), 0xFFFFFFFF, /*data_cnt=*/2, /*tgtid=*/0);
    mgr_->process_txdat(mst, dbid, 0, 6, flit_seg3.data(), 0xFFFFFFFF, /*data_cnt=*/2, /*tgtid=*/0);

    // 读回全 cacheline（size=7 → 4 flits）
    mgr_->process_txreq(mst, rtxn, addr, 7,
                        static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                        0xF, 200, true);

    auto zero = make_flit(uint8_t(0));
    mgr_->process_rxdat(mst, rtxn, 0, 0, zero.data(),       0xFFFFFFFF, 0);
    mgr_->process_rxdat(mst, rtxn, 0, 2, flit_seg1.data(),  0xFFFFFFFF, 0);
    mgr_->process_rxdat(mst, rtxn, 0, 4, zero.data(),       0xFFFFFFFF, 0);
    mgr_->process_rxdat(mst, rtxn, 0, 6, flit_seg3.data(),  0xFFFFFFFF, 0);

    auto s = mgr_->get_checker().get_stats();
    EXPECT_EQ(s.passes, 1);
    EXPECT_EQ(s.errors, 0);
}

// ============================================================
// 多 master 交叉：两个 master 同时有 outstanding 事务
// ============================================================
TEST_F(IntegrationTest, MultiMasterInterleaved) {
    uint32_t mst_a = 0, mst_b = 8;
    Addr_t addr = 0xB000;

    // Master A: 写 txnid=1
    mgr_->process_txreq(mst_a, 1, addr, 5,
                        static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull),
                        0x1, 100, false);
    // Master B: 写 txnid=1 (same txnid, different mst → different key)
    mgr_->process_txreq(mst_b, 1, addr + 128, 5,
                        static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull),
                        0x1, 101, false);

    // DBID responses interleaved
    mgr_->process_rxrsp_dbid(mst_b, 1, 0, /*dbid=*/200, /*srcid=*/0);
    mgr_->process_rxrsp_dbid(mst_a, 1, 0, /*dbid=*/201, /*srcid=*/0);

    // Data interleaved
    auto wd_a = make_flit(0xAA);
    auto wd_b = make_flit(0xBB);
    mgr_->process_txdat(mst_b, 200, 0, 0, wd_b.data(), 0xFFFFFFFF, 0, /*tgtid=*/0);
    mgr_->process_txdat(mst_a, 201, 0, 0, wd_a.data(), 0xFFFFFFFF, 0, /*tgtid=*/0);

    // Read back from each address
    mgr_->process_txreq(mst_a, 2, addr, 5,
                        static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                        0x1, 300, false);
    mgr_->process_rxdat(mst_a, 2, 0, 0, wd_a.data(), 0xFFFFFFFF, 0);

    mgr_->process_txreq(mst_b, 2, addr + 128, 5,
                        static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp),
                        0x1, 301, false);
    mgr_->process_rxdat(mst_b, 2, 0, 0, wd_b.data(), 0xFFFFFFFF, 0);

    auto s = mgr_->get_checker().get_stats();
    EXPECT_EQ(s.passes, 2);
    EXPECT_EQ(s.errors, 0);
}

// ============================================================
// size_to_flits 辅助函数验证
// ============================================================
TEST(TypesTest, SizeToFlits) {
    EXPECT_EQ(size_to_flits(0), 1);
    EXPECT_EQ(size_to_flits(1), 1);
    EXPECT_EQ(size_to_flits(5), 1);
    EXPECT_EQ(size_to_flits(6), 2);
    EXPECT_EQ(size_to_flits(7), 4);
}

// ============================================================
// dataid_to_offset 辅助函数验证
// ============================================================
TEST(TypesTest, DataidToOffset) {
    EXPECT_EQ(dataid_to_offset(0), 0);
    EXPECT_EQ(dataid_to_offset(2), 32);
    EXPECT_EQ(dataid_to_offset(4), 64);
    EXPECT_EQ(dataid_to_offset(6), 96);
}

// ============================================================
// secvec_to_flits 辅助函数验证
// ============================================================
TEST(TypesTest, SecvecToFlits) {
    EXPECT_EQ(secvec_to_flits(0x0), 0);
    EXPECT_EQ(secvec_to_flits(0x1), 1);
    EXPECT_EQ(secvec_to_flits(0x3), 2);
    EXPECT_EQ(secvec_to_flits(0x5), 2);
    EXPECT_EQ(secvec_to_flits(0xA), 2);
    EXPECT_EQ(secvec_to_flits(0x7), 3);
    EXPECT_EQ(secvec_to_flits(0xF), 4);
}
