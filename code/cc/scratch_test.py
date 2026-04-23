import os

path = r'd:\tpu\Work\Prj\dpi_mm_gm\code\cc\test\unit_test_shadow_mem.cc'
with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

# Replace make_write_txn signature
content = content.replace(
'''    Transaction make_write_txn(MstId_t mst, Addr_t addr,
                               Data_t data, Timestamp_t time,
                               SeqNum_t seq, TxnId_t id) {
        Transaction txn;
        txn.txn_id     = id;
        txn.global_seq = seq;
        txn.master_id  = mst;''',
'''    Transaction make_write_txn(uint32_t mst, Addr_t addr,
                               Data_t data, Timestamp_t time,
                               SeqNum_t seq, TxnId_t id) {
        Transaction txn;
        txn.txn_id     = id;
        txn.global_seq = seq;
        txn.src_id     = mst;
        txn.tgt_id     = 0;
        txn.dbid       = 0;'''
)

# Replace write_byte calls with write({value}, {true}, ...)
import re

def repl_write_byte(m):
    # m.group(1)=addr, m.group(2)=value, m.group(3)=mst, m.group(4)=txn, m.group(5)=seq, m.group(6)=time
    return f'mem_->write({m.group(1)}, {{{m.group(2)}}}, {{true}}, {m.group(3)}, 0, {m.group(4)}, {m.group(5)}, {m.group(6)})'

content = re.sub(r'mem_->write_byte\(\s*(.*?),\s*(.*?),\s*(.*?),\s*(.*?),\s*(.*?),\s*(.*?)\s*\)', repl_write_byte, content)

# Also replace /*mst*/0 etc if any
content = content.replace('/*mst*/0', '0')
content = content.replace('/*txn*/1', '1')
content = content.replace('/*seq*/0', '0')
content = content.replace('/*time*/100', '100')

# write(...) has src_id, tgt_id. So we need 0, 1, 0, 100 -> src=0, tgt=0, txn=1, seq=0, time=100.
# The previous mem_->write(0x1000, data, be, 0, 1, 0, 100); -> addr, data, be, mst=0, txn=1, seq=0, time=100
# now needs to be mem_->write(0x1000, data, be, src=0, tgt=0, txn=1, seq=0, time=100);
content = content.replace('mem_->write(0x1000, data, be, 0, 1, 0, 100);', 'mem_->write(0x1000, data, be, 0, 0, 1, 0, 100);')
content = content.replace('mem_->write(cross_addr, data, be, 0, 1, 0, 100);', 'mem_->write(cross_addr, data, be, 0, 0, 1, 0, 100);')

# read_and_log
content = content.replace('mem_->read_and_log(0xD000, 2, /*mst*/1, /*txn*/10, /*time*/300)', 'mem_->read_and_log(0xD000, 2, 1, 10, 300)')

# last_wr->master_id -> last_wr->src_id
content = content.replace('last_wr->master_id', 'last_wr->src_id')

# for_each_written_byte -> for_each_written_block
content = content.replace('for_each_written_byte', 'for_each_written_block')
content = content.replace('const ByteSlot& slot', 'const BlockSlot& slot')

# collected[addr] = slot.current_value; -> collected[addr] = slot.current_value[0];
# But wait, for_each_written_block gives block_addr and BlockSlot. The test collects addr.
# If block_addr is passed, we shouldn't just take [0]. The test wrote 0xA000, 0xA001, 0xB000.
content = content.replace(
'''    mem_->for_each_written_block([&](Addr_t addr, const BlockSlot& slot) {
        collected[addr] = slot.current_value;
        count++;
    });''',
'''    mem_->for_each_written_block([&](Addr_t addr, const BlockSlot& slot) {
        // Collect all written bytes in the block
        for(int i=0; i<32; ++i) {
            if(slot.init_mask & (1<<i)) {
                collected[addr + i] = slot.current_value[i];
                count++;
            }
        }
    });'''
)

# Test 13 history: The history stores array of 32 bytes.
# EXPECT_EQ(hist[0].value, 0x11); -> EXPECT_EQ(hist[0].value[0], 0x11);
# Actually the test wrote to 0x8000, which has offset 0.
content = content.replace('EXPECT_EQ(hist[0].value, 0x11);', 'EXPECT_EQ(hist[0].value[0], 0x11);')
content = content.replace('EXPECT_EQ(hist[1].value, 0x22);', 'EXPECT_EQ(hist[1].value[0], 0x22);')
content = content.replace('EXPECT_EQ(hist[2].value, 0x33);', 'EXPECT_EQ(hist[2].value[0], 0x33);')
content = content.replace('EXPECT_EQ(hist.front().value, 12);', 'EXPECT_EQ(hist.front().value[0], 12);')
content = content.replace('EXPECT_EQ(hist.back().value, 19);', 'EXPECT_EQ(hist.back().value[0], 19);')

with open(path, 'w', encoding='utf-8') as f:
    f.write(content)

print("Done unit_test_shadow_mem.cc")
