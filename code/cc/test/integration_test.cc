// ============================================================
// file: test/integration_test.cc
// ============================================================
#include <gtest/gtest.h>
#include "chi_transaction_manager.h"
#include <array>

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // strict mode
        mgr_ = std::make_unique<ChiTransactionManager>(true);
    }

    std::unique_ptr<ChiTransactionManager> mgr_;
};

TEST_F(IntegrationTest, NormalWriteReadFlow) {
    uint32_t mst_idx = 1;
    uint32_t write_txnid = 10;
    uint32_t read_txnid = 11;
    uint32_t dbid = 50;
    Addr_t addr = 0x1000;
    
    // Write flow
    // 1. Req -> Home (WriteNoSnpFull)
    mgr_->process_txreq(mst_idx, write_txnid, addr, 128, static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull), 1, 100);
    
    // 2. Home -> Req (DBIDResp)
    mgr_->process_rxrsp_dbid(mst_idx, write_txnid, 0, dbid);
    
    // 3. Req -> Home (NCBWrData)
    std::array<uint8_t, 32> w_data = {0};
    w_data[0] = 0xDE; w_data[1] = 0xAD; w_data[2] = 0xBE; w_data[3] = 0xEF;
    mgr_->process_txdat(mst_idx, dbid, 0, 0, w_data.data(), 0xF, 1);

    // Read flow
    // 1. Req -> Home (ReadNoSnp)
    mgr_->process_txreq(mst_idx, read_txnid, addr, 128, static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp), 1, 200);

    // 2. Home -> Req (CompData)
    mgr_->process_rxdat(mst_idx, read_txnid, 0, 0, w_data.data(), 0xF, 1);
    
    auto stats = mgr_->get_checker().get_stats();
    EXPECT_EQ(stats.passes, 1);
    EXPECT_EQ(stats.errors, 0);
}

TEST_F(IntegrationTest, ReadErrorFlow) {
    uint32_t mst_idx = 1;
    uint32_t read_txnid = 12;
    Addr_t addr = 0x2000;
    
    std::array<uint8_t, 32> r_data = {0};
    r_data[0] = 0x99; r_data[1] = 0x99;

    // 1. Req -> Home (ReadNoSnp)
    mgr_->process_txreq(mst_idx, read_txnid, addr, 128, static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp), 1, 100);

    // 2. Home -> Req (CompData)
    mgr_->process_rxdat(mst_idx, read_txnid, 0, 0, r_data.data(), 0x3, 1);
    
    auto stats = mgr_->get_checker().get_stats();
    EXPECT_EQ(stats.errors, 1);
}

TEST_F(IntegrationTest, PartialWriteReadFlow) {
    uint32_t mst_idx = 1;
    uint32_t write_txnid = 20;
    uint32_t read_txnid = 21;
    uint32_t dbid = 60;
    Addr_t addr = 0x3000;
    
    // Write partial (WriteNoSnpPtl)
    mgr_->process_txreq(mst_idx, write_txnid, addr, 128, static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpPtl), 1, 100);
    mgr_->process_rxrsp_dbid(mst_idx, write_txnid, 0, dbid);
    
    std::array<uint8_t, 32> w_data = {0};
    w_data[0] = 0xAA; w_data[1] = 0xBB; w_data[2] = 0xCC; w_data[3] = 0xDD;
    // Only enable byte 0 and 2
    mgr_->process_txdat(mst_idx, dbid, 0, 0, w_data.data(), 0x5, 1);

    // Read check
    mgr_->process_txreq(mst_idx, read_txnid, addr, 128, static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp), 1, 200);

    std::array<uint8_t, 32> r_data = {0};
    r_data[0] = 0xAA; r_data[1] = 0x00; r_data[2] = 0xCC; r_data[3] = 0x00;
    // The expected is 0xAA, 0x00, 0xCC, 0x00 because 1 and 3 were not written and default is 0x00
    mgr_->process_rxdat(mst_idx, read_txnid, 0, 0, r_data.data(), 0xF, 1);

    auto stats = mgr_->get_checker().get_stats();
    EXPECT_EQ(stats.passes, 1);
    EXPECT_EQ(stats.errors, 0);
}

TEST_F(IntegrationTest, StressTestFlow) {
    const int NUM_TXNS = 100;
    uint32_t mst_idx = 2;
    
    // Issue 100 Writes
    for (int i = 0; i < NUM_TXNS; ++i) {
        uint32_t txnid = 100 + i;
        uint32_t dbid = 1000 + i;
        Addr_t addr = 0x4000 + (i * 128);
        
        mgr_->process_txreq(mst_idx, txnid, addr, 128, static_cast<uint32_t>(ChiReqOpcode::WriteNoSnpFull), 1, 100 + i);
        mgr_->process_rxrsp_dbid(mst_idx, txnid, 0, dbid);
        
        std::array<uint8_t, 32> w_data = {0};
        w_data[0] = static_cast<uint8_t>(i & 0xFF);
        mgr_->process_txdat(mst_idx, dbid, 0, 0, w_data.data(), 0x1, 1);
    }
    
    // Issue 100 Reads to verify
    for (int i = 0; i < NUM_TXNS; ++i) {
        uint32_t txnid = 200 + i;
        Addr_t addr = 0x4000 + (i * 128);
        
        mgr_->process_txreq(mst_idx, txnid, addr, 128, static_cast<uint32_t>(ChiReqOpcode::ReadNoSnp), 1, 1000 + i);
        
        std::array<uint8_t, 32> r_data = {0};
        r_data[0] = static_cast<uint8_t>(i & 0xFF);
        mgr_->process_rxdat(mst_idx, txnid, 0, 0, r_data.data(), 0x1, 1);
    }
    
    auto stats = mgr_->get_checker().get_stats();
    EXPECT_EQ(stats.passes, NUM_TXNS);
    EXPECT_EQ(stats.errors, 0);
}
