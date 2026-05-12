// ============================================================
// file: test/unit_test_shadow_mem.cpp
// ============================================================
#include <gtest/gtest.h>
#include "shadow_memory.h"

class ShadowMemoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        ShadowMemoryConfig cfg;
        cfg.max_write_history  = 8;
        cfg.default_init_value = 0x00;
        cfg.page_size_bits     = 12; // 4KB pages
        mem_ = std::make_unique<ShadowMemory>(cfg);
    }

    std::unique_ptr<ShadowMemory> mem_;

    // 辅助：构造简单Transaction
    Transaction make_write_txn(uint32_t src_id, Addr_t addr,
                               Data_t data, Timestamp_t time,
                               SeqNum_t seq, TxnId_t id) {
        Transaction txn;
        txn.txn_id     = id;
        txn.global_seq = seq;
        txn.src_id     = src_id;
        txn.tgt_id     = 0;
        txn.dbid       = 0;
        txn.type       = TxnType::WRITE;
        txn.addr       = align_to_cacheline(addr);
        txn.size       = CHI_CL_BYTES;
        txn.burst_len  = 1;
        txn.secvec     = 0xF;
        
        txn.data.assign(CHI_CL_BYTES, 0);
        txn.byte_enable.assign(CHI_CL_BYTES, false);
        uint32_t offset = addr & (CHI_CL_BYTES - 1);
        for (size_t i = 0; i < data.size() && offset + i < CHI_CL_BYTES; ++i) {
            txn.data[offset + i] = data[i];
            txn.byte_enable[offset + i] = true;
        }

        txn.req_time   = time - 5;
        txn.resp_time  = time;
        txn.status     = TxnStatus::COMPLETED;
        return txn;
    }
};

// ---- 测试1：基本写读 ----
TEST_F(ShadowMemoryTest, BasicWriteRead) {
    mem_->write(0x1000, {0xAB}, {true}, 0, 0, 1, 0, 100);

    auto val = mem_->read_byte(0x1000);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val.value(), 0xAB);
}

// ---- 测试2：未写地址返回nullopt ----
TEST_F(ShadowMemoryTest, ReadUnwrittenReturnsNullopt) {
    auto val = mem_->read_byte(0x2000);
    EXPECT_FALSE(val.has_value());
}

// ---- 测试3：多次写同一地址，保留历史 ----
TEST_F(ShadowMemoryTest, WriteHistoryTracking) {
    mem_->write(0x1000, {0x11}, {true}, 0, 0, 1, 0, 100);
    mem_->write(0x1000, {0x22}, {true}, 1, 0, 2, 1, 200);
    mem_->write(0x1000, {0x33}, {true}, 0, 0, 3, 2, 300);

    auto val = mem_->read_byte(0x1000);
    EXPECT_EQ(val.value(), 0x33); // 最新值

    const auto& hist = mem_->get_write_history(0x1000);
    ASSERT_EQ(hist.size(), 3u);
    EXPECT_EQ(hist[0].value[0], 0x11);
    EXPECT_EQ(hist[1].value[0], 0x22);
    EXPECT_EQ(hist[2].value[0], 0x33);
}

// ---- 测试4：Byte enable部分写 ----
TEST_F(ShadowMemoryTest, PartialWriteWithByteEnable) {
    Data_t   data = {0xAA, 0xBB, 0xCC, 0xDD};
    ByteEn_t be   = {true, false, true, false};

    mem_->write(0x1000, data, be, 0, 0, 1, 0, 100);

    EXPECT_EQ(mem_->read_byte(0x1000).value(), 0xAA);  // enabled
    EXPECT_FALSE(mem_->has_been_written(0x1001));        // disabled
    EXPECT_EQ(mem_->read_byte(0x1002).value(), 0xCC);  // enabled
    EXPECT_FALSE(mem_->has_been_written(0x1003));        // disabled
}

// ---- 测试5：时间窗口内可能值 ----
TEST_F(ShadowMemoryTest, PossibleValuesInTimeWindow) {
    mem_->write(0x1000, {0x11}, {true}, 0, 0, 1, 0, 100);
    mem_->write(0x1000, {0x22}, {true}, 1, 0, 2, 1, 200);
    mem_->write(0x1000, {0x33}, {true}, 0, 0, 3, 2, 300);

    // 窗口[150, 250]应该包含 0x11(窗口前最后写) 和 0x22(窗口内)
    auto vals = mem_->get_possible_values(0x1000, 150, 250);
    EXPECT_NE(std::find(vals.begin(), vals.end(), 0x11), vals.end());
    EXPECT_NE(std::find(vals.begin(), vals.end(), 0x22), vals.end());
    EXPECT_EQ(std::find(vals.begin(), vals.end(), 0x33), vals.end());
}

// ---- 测试6：Transaction接口写入 ----
TEST_F(ShadowMemoryTest, WriteFromTransaction) {
    auto txn = make_write_txn(0, 0x2000, {0xDE, 0xAD, 0xBE, 0xEF}, 500, 10, 99);
    mem_->write(txn);

    Data_t readback = mem_->read(0x2000, 4);
    EXPECT_EQ(readback[0], 0xDE);
    EXPECT_EQ(readback[1], 0xAD);
    EXPECT_EQ(readback[2], 0xBE);
    EXPECT_EQ(readback[3], 0xEF);

    // 验证写历史中的src_id和txn_id
    auto last_wr = mem_->get_last_write(0x2000);
    ASSERT_TRUE(last_wr.has_value());
    EXPECT_EQ(last_wr->src_id, 0u);
    EXPECT_EQ(last_wr->txn_id, 99u);
    EXPECT_EQ(last_wr->global_seq, 10u);
}

// ---- 测试7：preload不产生写历史 ----
TEST_F(ShadowMemoryTest, PreloadNoWriteHistory) {
    mem_->preload(0x3000, {0x01, 0x02, 0x03, 0x04});

    EXPECT_TRUE(mem_->has_been_written(0x3000));
    EXPECT_EQ(mem_->read_byte(0x3000).value(), 0x01);

    // preload不应产生写历史
    const auto& hist = mem_->get_write_history(0x3000);
    EXPECT_TRUE(hist.empty());
}

// ---- 测试8：invalidate_range清除数据和历史 ----
TEST_F(ShadowMemoryTest, InvalidateRange) {
    mem_->write(0x4000, {0xAA}, {true}, 0, 0, 1, 0, 100);
    mem_->write(0x4001, {0xBB}, {true}, 0, 0, 2, 1, 200);

    EXPECT_TRUE(mem_->has_been_written(0x4000));
    EXPECT_TRUE(mem_->has_been_written(0x4001));

    mem_->invalidate_range(0x4000, 2);

    EXPECT_FALSE(mem_->has_been_written(0x4000));
    EXPECT_FALSE(mem_->has_been_written(0x4001));
    EXPECT_TRUE(mem_->get_write_history(0x4000).empty());
}

// ---- 测试9：reset全部清空 ----
TEST_F(ShadowMemoryTest, ResetClearsEverything) {
    mem_->write(0x5000, {0xFF}, {true}, 0, 0, 1, 0, 100);
    EXPECT_TRUE(mem_->has_been_written(0x5000));
    EXPECT_GT(mem_->get_total_pages_allocated(), 0u);

    mem_->reset();

    EXPECT_FALSE(mem_->has_been_written(0x5000));
    EXPECT_EQ(mem_->get_total_pages_allocated(), 0u);
}

// ---- 测试10：跨page边界写读 ----
TEST_F(ShadowMemoryTest, CrossPageBoundary) {
    // page_size_bits=12, 即4KB page, page边界在0x1000
    Addr_t cross_addr = 0x0FFE; // 最后2 byte在page0, 前2 byte在page1
    Data_t data = {0xAA, 0xBB, 0xCC, 0xDD};
    ByteEn_t be = {true, true, true, true};

    mem_->write(cross_addr, data, be, 0, 0, 1, 0, 100);

    EXPECT_EQ(mem_->read_byte(0x0FFE).value(), 0xAA); // page 0
    EXPECT_EQ(mem_->read_byte(0x0FFF).value(), 0xBB); // page 0
    EXPECT_EQ(mem_->read_byte(0x1000).value(), 0xCC); // page 1
    EXPECT_EQ(mem_->read_byte(0x1001).value(), 0xDD); // page 1

    // 应该分配了2个page
    EXPECT_EQ(mem_->get_total_pages_allocated(), 2u);
}

// ---- 测试11：get_value_at_time回溯查询 ----
TEST_F(ShadowMemoryTest, GetValueAtTime) {
    mem_->write(0x6000, {0x11}, {true}, 0, 0, 1, 0, 100);
    mem_->write(0x6000, {0x22}, {true}, 1, 0, 2, 1, 200);
    mem_->write(0x6000, {0x33}, {true}, 0, 0, 3, 2, 300);

    // 在t=150时，只有第一次写(t=100)已完成
    auto val_150 = mem_->get_value_at_time(0x6000, 150);
    ASSERT_TRUE(val_150.has_value());
    EXPECT_EQ(val_150.value(), 0x11);

    // 在t=250时，前两次写都已完成，应返回最新的0x22
    auto val_250 = mem_->get_value_at_time(0x6000, 250);
    ASSERT_TRUE(val_250.has_value());
    EXPECT_EQ(val_250.value(), 0x22);

    // 在t=50时，没有任何写完成
    auto val_50 = mem_->get_value_at_time(0x6000, 50);
    EXPECT_FALSE(val_50.has_value());
}

// ---- 测试12：is_range_written检查 ----
TEST_F(ShadowMemoryTest, IsRangeWritten) {
    mem_->write(0x7000, {0xAA}, {true}, 0, 0, 1, 0, 100);
    mem_->write(0x7001, {0xBB}, {true}, 0, 0, 2, 1, 100);
    // 0x7002未写

    EXPECT_TRUE(mem_->is_range_written(0x7000, 2));
    EXPECT_FALSE(mem_->is_range_written(0x7000, 3));
}

// ---- 测试13：写历史环形缓冲上限 ----
TEST_F(ShadowMemoryTest, WriteHistoryRingBufferLimit) {
    // config中max_write_history=8
    for (uint32_t i = 0; i < 20; ++i) {
        mem_->write(0x8000, {static_cast<uint8_t>(i)}, {true}, 0, 0, i, i, 100 + i * 10);
    }

    const auto& hist = mem_->get_write_history(0x8000);
    EXPECT_EQ(hist.size(), 8u); // 最多保留8条

    // 最旧的应该是第12次写(i=12)
    EXPECT_EQ(hist.front().value[0], 12);
    // 最新的应该是第19次写(i=19)
    EXPECT_EQ(hist.back().value[0], 19);
}

// ---- 测试14：dump输出不崩溃 ----
TEST_F(ShadowMemoryTest, DumpDoesNotCrash) {
    mem_->write(0x9000, {0xAB}, {true}, 0, 0, 1, 0, 100);

    std::string range_dump = mem_->dump_range(0x9000, 32);
    EXPECT_FALSE(range_dump.empty());

    std::string hist_dump = mem_->dump_write_history(0x9000);
    EXPECT_FALSE(hist_dump.empty());

    std::string stats_dump = mem_->dump_stats();
    EXPECT_FALSE(stats_dump.empty());

    // 打印到stdout方便人工查看
    std::cout << range_dump << std::endl;
    std::cout << hist_dump << std::endl;
    std::cout << stats_dump << std::endl;
}

// ---- 测试15：for_each_written_block遍历 ----
TEST_F(ShadowMemoryTest, ForEachWrittenBlock) {
    mem_->write(0xA000, {0x11}, {true}, 0, 0, 1, 0, 100);
    mem_->write(0xA001, {0x22}, {true}, 0, 0, 2, 1, 200);
    mem_->write(0xB000, {0x33}, {true}, 1, 0, 3, 2, 300);

    uint64_t count = 0;
    std::map<Addr_t, uint8_t> collected;

    mem_->for_each_written_block([&](Addr_t addr, const BlockSlot& slot) {
        for(int i=0; i<32; ++i) {
            if(slot.init_mask & (1U<<i)) {
                collected[addr + i] = slot.current_value[i];
                count++;
            }
        }
    });

    EXPECT_EQ(count, 3u);
    EXPECT_EQ(collected[0xA000], 0x11);
    EXPECT_EQ(collected[0xA001], 0x22);
    EXPECT_EQ(collected[0xB000], 0x33);
}

// ---- 测试16：read连续区域混合已写和未写byte ----
TEST_F(ShadowMemoryTest, ReadMixedInitializedRange) {
    mem_->write(0xC000, {0xAA}, {true}, 0, 0, 1, 0, 100);
    // 0xC001 未写
    mem_->write(0xC002, {0xCC}, {true}, 0, 0, 2, 1, 200);
    // 0xC003 未写

    Data_t result = mem_->read(0xC000, 4);
    EXPECT_EQ(result[0], 0xAA);
    EXPECT_EQ(result[1], 0x00); // default_init_value
    EXPECT_EQ(result[2], 0xCC);
    EXPECT_EQ(result[3], 0x00); // default_init_value
}

// ---- 测试17：read_and_log记录读日志 ----
TEST_F(ShadowMemoryTest, ReadAndLogTracksEntries) {
    // 重新创建启用read log的实例
    ShadowMemoryConfig cfg;
    cfg.max_write_history  = 8;
    cfg.default_init_value = 0x00;
    cfg.page_size_bits     = 12;
    cfg.track_read_log     = true;  // 启用读日志
    mem_ = std::make_unique<ShadowMemory>(cfg);

    mem_->write(0xD000, {0x55}, {true}, 0, 0, 1, 0, 100);
    mem_->write(0xD001, {0x66}, {true}, 0, 0, 2, 1, 200);

    Data_t result = mem_->read_and_log(0xD000, 2, 1, 10, 300);

    EXPECT_EQ(result[0], 0x55);
    EXPECT_EQ(result[1], 0x66);

    // 检查统计中包含读日志信息
    std::string stats = mem_->dump_stats();
    EXPECT_NE(stats.find("Read log entries"), std::string::npos);
}