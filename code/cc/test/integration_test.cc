// ============================================================
// file: test/integration_test.cc
// ============================================================
#include <gtest/gtest.h>
#include "chi_transaction_manager.h"

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // strict mode
        mgr_ = std::make_unique<ChiTransactionManager>(true);
    }

    std::unique_ptr<ChiTransactionManager> mgr_;
};

TEST_F(IntegrationTest, NormalWriteReadFlow) {
    // Req -> Home
    mgr_->process_txreq_flit(1, 2, 10, 0x1000, 4, 1, 100);
    
    // Home -> Req (Rxrsp dbid)
    mgr_->process_rxrsp_flit(2, 1, 10, 50); // Src=Home, Tgt=Req, txn=10, dbid=50
    
    // Req -> Home (Txdat)
    Data_t w_data = {0xDE, 0xAD, 0xBE, 0xEF};
    ByteEn_t w_be(4, true);
    mgr_->process_txdat_flit(1, 2, 50, w_data, w_be, 0, 150);

    // Read check
    mgr_->process_read_completed(1, 2, 11, 0x1000, 4, 1, w_data, 200, 250);
    
    auto stats = mgr_->get_checker().get_stats();
    EXPECT_EQ(stats.passes, 1);
    EXPECT_EQ(stats.errors, 0);
}

TEST_F(IntegrationTest, ReadErrorFlow) {
    Data_t r_data = {0x99, 0x99};
    mgr_->process_read_completed(1, 2, 12, 0x2000, 2, 1, r_data, 50, 100);
    
    auto stats = mgr_->get_checker().get_stats();
    EXPECT_EQ(stats.errors, 1);
}
