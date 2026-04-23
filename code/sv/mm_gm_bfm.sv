


module mm_gm_bfm (
    
);

    // ---- 初始化、请求、响应的接口均无变化 ----
    import "DPI-C" function void refmodel_init(input int strict_mode);

    import "DPI-C" function void dpi_chi_txreq_write(
        input int src_id, 
        input int tgt_id, 
        input int txn_id,
        input longint addr, 
        input int size, 
        input int burst_len,
        input longint req_time
    );

    import "DPI-C" function void dpi_chi_rxrsp_dbid(
        input int src_id, 
        input int tgt_id, 
        input int txn_id, 
        input int dbid
    );

    // ==========================================
    // 重构后的高效 Data 传输接口：
    // ==========================================

    // 发送写数据 flit (原生 256-bit data + 32-bit byte_en)
    import "DPI-C" function void dpi_chi_txdat(
        input int src_id, 
        input int tgt_id, 
        input int dbid,
        input bit [255:0] data_bits,   // 直接传宽总线变量！
        input bit [31:0]  byte_en_bits, // 直接传宽总线变量！
        input int offset, 
        input longint resp_time
    );

    // 验证读数据 (原生变长 bit 数组)
    // 这里的 resp_data_bits 需要声明为足够大的宽位，
    // 比如单次最大支持512-bit，你可以写 input bit [511:0] resp_data_bits。
    // C++ 底层会根据你传入的 size * burst_len 自动计算并只读取对应的有效字节！
    import "DPI-C" function int dpi_chi_read_check(
        input int src_id, 
        input int tgt_id, 
        input int txn_id,
        input longint addr, 
        input int size, 
        input int burst_len,
        input bit [511:0] resp_data_bits, // 或者 [255:0]，取决于你设定的最大单次传输范围
        input longint req_time, 
        input longint resp_time
    );



endmodule