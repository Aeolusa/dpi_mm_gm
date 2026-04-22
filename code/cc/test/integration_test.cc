// ============================================================
// file: test/integration_test.cc
// ============================================================
#include <gtest/gtest.h>
#include "transaction_manager.h"

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 2 masters, strict mode
        mgr_ = std::make_unique<TransactionManager>(2, true);
    }

    std::unique_ptr<TransactionManager> mgr_;
};

TEST_F(IntegrationTest, NormalWriteReadFlow) {
    // MST 0 submits write to 0x1000
    Data_t w_data = {0xDE, 0xAD, 0xBE, 0xEF};
    ByteEn_t w_be(4, true);
    TxnId_t w_id = mgr_->submit_request(0, TxnType::WRITE, 0x1000, 4, 1, w_data, w_be, 100);

    // Ensure it was assigned id 0
    EXPECT_EQ(w_id, 0);

    // Complete write
    auto report_w = mgr_->complete_transaction(0, w_id, {}, 150);
    EXPECT_EQ(report_w.result, CheckResult::PASS);

    // MST 1 submits read from 0x1000
    ByteEn_t r_be(4, true);
    TxnId_t r_id = mgr_->submit_request(1, TxnType::READ, 0x1000, 4, 1, {}, r_be, 200);

    // Provide read response
    auto report_r = mgr_->complete_transaction(1, r_id, w_data, 250);
    EXPECT_EQ(report_r.result, CheckResult::PASS);
}

TEST_F(IntegrationTest, ReadErrorFlow) {
    // MST 0 submits read from unwritten memory 0x2000
    ByteEn_t r_be(2, true);
    TxnId_t r_id = mgr_->submit_request(0, TxnType::READ, 0x2000, 2, 1, {}, r_be, 50);

    Data_t r_data = {0x99, 0x99};
    auto report = mgr_->complete_transaction(0, r_id, r_data, 100);

    EXPECT_EQ(report.result, CheckResult::FAIL_NO_PRIOR_WRITE);
}
