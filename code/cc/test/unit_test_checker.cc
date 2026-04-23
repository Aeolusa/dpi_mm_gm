// ============================================================
// file: test/unit_test_checker.cc
// ============================================================
#include <gtest/gtest.h>
#include "consistency_check.h"

class CheckerTest : public ::testing::Test {
protected:
    void SetUp() override {
        checker_ = std::make_unique<ConsistencyChecker>(true);
    }

    std::unique_ptr<ConsistencyChecker> checker_;

    Transaction make_write_txn(MstId_t mst, Addr_t addr, Data_t data, 
                               Timestamp_t req_time, Timestamp_t resp_time, 
                               SeqNum_t seq, TxnId_t id) {
        Transaction txn;
        txn.txn_id     = id;
        txn.global_seq = seq;
        txn.src_id     = mst;
        txn.tgt_id     = 0;
        txn.dbid       = 0;
        txn.type       = TxnType::WRITE;
        txn.addr       = addr;
        txn.size       = static_cast<uint32_t>(data.size());
        txn.burst_len  = 1;
        txn.data       = std::move(data);
        txn.byte_enable.assign(txn.size, true);
        txn.req_time   = req_time;
        txn.resp_time  = resp_time;
        txn.status     = TxnStatus::COMPLETED;
        return txn;
    }

    Transaction make_read_txn(MstId_t mst, Addr_t addr, Data_t data, 
                              Timestamp_t req_time, Timestamp_t resp_time, 
                              SeqNum_t seq, TxnId_t id) {
        Transaction txn;
        txn.txn_id     = id;
        txn.global_seq = seq;
        txn.src_id     = mst;
        txn.tgt_id     = 0;
        txn.dbid       = 0;
        txn.type       = TxnType::READ;
        txn.addr       = addr;
        txn.size       = static_cast<uint32_t>(data.size());
        txn.burst_len  = 1;
        txn.data       = std::move(data);
        txn.byte_enable.assign(txn.size, true);
        txn.req_time   = req_time;
        txn.resp_time  = resp_time;
        txn.status     = TxnStatus::COMPLETED;
        return txn;
    }
};

TEST_F(CheckerTest, BasicWriteReadPass) {
    auto w_txn = make_write_txn(0, 0x100, {0xAA, 0xBB}, 10, 20, 1, 1);
    checker_->process_write(w_txn);

    auto r_txn = make_read_txn(1, 0x100, {0xAA, 0xBB}, 30, 40, 2, 2);
    auto report = checker_->process_read(r_txn);

    EXPECT_EQ(report.result, CheckResult::PASS);
    EXPECT_EQ(checker_->get_stats().passes, 1);
    EXPECT_EQ(checker_->get_stats().errors, 0);
}

TEST_F(CheckerTest, NoPriorWriteFail) {
    auto r_txn = make_read_txn(1, 0x200, {0x11}, 30, 40, 2, 2);
    auto report = checker_->process_read(r_txn);

    EXPECT_EQ(report.result, CheckResult::FAIL_NO_PRIOR_WRITE);
    EXPECT_EQ(report.fail_addr, 0x200);
    EXPECT_EQ(checker_->get_stats().errors, 1);
}

TEST_F(CheckerTest, DataMismatchFail) {
    auto w_txn = make_write_txn(0, 0x300, {0xAA}, 10, 20, 1, 1);
    checker_->process_write(w_txn);

    auto r_txn = make_read_txn(1, 0x300, {0xBB}, 30, 40, 2, 2);
    auto report = checker_->process_read(r_txn);

    EXPECT_EQ(report.result, CheckResult::FAIL_DATA_MISMATCH);
    EXPECT_EQ(report.expected, 0xAA);
    EXPECT_EQ(report.actual, 0xBB);
    EXPECT_EQ(checker_->get_stats().errors, 1);
}

TEST_F(CheckerTest, TimeOverlapCheckPass) {
    // Write 1 completed at 20
    auto w1 = make_write_txn(0, 0x400, {0x11}, 10, 20, 1, 1);
    checker_->process_write(w1);
    
    // Write 2 overlaps with Read Request 
    // Write 2 req=25, resp=35
    auto w2 = make_write_txn(1, 0x400, {0x22}, 25, 35, 2, 2);
    checker_->process_write(w2);

    // Read req=30, resp=40
    // At req=30, w1 is completely done. w2 is ongoing.
    // The overlap window is 10. w2 completed at 35, which is inside [30-10, 40] 
    // so w2 should be considered overlapping and its value allowed.
    auto r_txn1 = make_read_txn(2, 0x400, {0x22}, 30, 40, 3, 3);
    auto report1 = checker_->process_read(r_txn1);
    EXPECT_EQ(report1.result, CheckResult::PASS);

    // Reading 0x11 should also be allowed because it was the value before overlap
    auto r_txn2 = make_read_txn(3, 0x400, {0x11}, 30, 40, 4, 4);
    auto report2 = checker_->process_read(r_txn2);
    EXPECT_EQ(report2.result, CheckResult::PASS);
}

TEST_F(CheckerTest, TimeOverlapCheckFail) {
    auto w1 = make_write_txn(0, 0x500, {0x11}, 10, 20, 1, 1);
    checker_->process_write(w1);
    
    auto w2 = make_write_txn(1, 0x500, {0x22}, 25, 35, 2, 2);
    checker_->process_write(w2);

    auto r_txn = make_read_txn(2, 0x500, {0x33}, 30, 40, 3, 3);
    auto report = checker_->process_read(r_txn);

    // Fails because actual is 0x33, which is neither 0x11 nor 0x22
    EXPECT_EQ(report.result, CheckResult::FAIL_DATA_MISMATCH);
}
