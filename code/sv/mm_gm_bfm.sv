// ============================================================
// file: sv/mm_gm_bfm.sv
// CHI BFM：监听单个 MST 接口的所有通道 flit
//   - 通过 MST_IDX 参数区分不同BFM实例
//   - 通过 IS_SM 参数标识 SM 类型 master
//   - 所有 DPI 函数新增 mst_idx 参数
// ============================================================
  
module mm_gm_bfm #(
    parameter int  MST_IDX                              = 0,    // BFM 实例编号
    parameter bit  IS_SM                                = 0,    // 是否为 SM 类型 master
    parameter IS_FABRIC_CHI                             = 0,
    parameter REQ_CHNS                                  = 1,
    parameter REQ_W                                     = 1,
    parameter TXRSP_CHNS                                = 1,
    parameter TXRSP_W                                   = 1,
    parameter RXRSP_CHNS                                = 1,
    parameter RXRSP_W                                   = 1, 
    parameter TXDAT_CHNS                                = 1,
    parameter TXDAT_W                                   = 1,
    parameter RXDAT_CHNS                                = 1,
    parameter RXDAT_W                                   = 1,
    // SNP channel (IS_FABRIC_CHI only)
    parameter RXSNP_CHNS                                = 1,
    parameter RXSNP_W                                   = 1 
) (
    input bit                                           clk,
    input bit                                           rstn,

    input logic [REQ_CHNS-1:0][REQ_W-1:0]               txreq_flit,
    input logic [REQ_CHNS-1:0]                          txreq_flitv,
    input logic [TXRSP_CHNS-1:0][TXRSP_W-1:0]           txrsp_flit,
    input logic [TXRSP_CHNS-1:0]                        txrsp_flitv,
    input logic [RXRSP_CHNS-1:0][RXRSP_W-1:0]           rxrsp_flit,
    input logic [RXRSP_CHNS-1:0]                        rxrsp_flitv,
    input logic [TXDAT_CHNS-1:0][TXDAT_W-1:0]           txdat_flit,
    input logic [TXDAT_CHNS-1:0]                        txdat_flitv,
    input logic [RXDAT_CHNS-1:0][RXDAT_W-1:0]           rxdat_flit,
    input logic [RXDAT_CHNS-1:0]                        rxdat_flitv,
    input logic [RXSNP_CHNS-1:0][RXSNP_W-1:0]           rxsnp_flit,
    input logic [RXSNP_CHNS-1:0]                        rxsnp_flitv,
    output logic                                        bfm_err_flag           
);

    // ---- DPI-C 函数声明 ----

    export "DPI-C" function sv_set_gm_error;
    function void sv_set_gm_error();
        bfm_err_flag = 1'b1;
    endfunction

    export "DPI-C" function sv_clr_gm_error;
    function void sv_clr_gm_error();
        bfm_err_flag = 1'b0;
    endfunction

    // txreq：读写请求统一入口（opcode 区分 RdNoSnp / WrNoSnp）
    //   新增 is_sm 参数，标识 SM 类型 master
    import "DPI-C" function void dpi_chi_txreq(
        input int mst_idx,
        input int txnid,
        input longint addr, 
        input int size, 
        input int opcode,
        input int secvec,
        input longint req_time,
        input int is_sm,
        input bit soft_ctrl,
        input bit is_fabric_chi,
        input bit [511:0] flit
    );

    // rxrsp：DBIDResp / CompDBIDResp
    import "DPI-C" function void dpi_chi_rxrsp_dbid(
        input int mst_idx,
        input int txnid, 
        input int opcode,
        input int dbid,
        input bit soft_ctrl,
        input bit is_fabric_chi,
        input bit [511:0] flit
    );

    // txdat：发送写数据 NCBWrData
    //   注意：txnid 字段在 CHI 中实际携带 dbid 值
    import "DPI-C" function void dpi_chi_txdat(
        input int mst_idx,
        input int txnid,
        input int opcode,
        input int dataid,   
        input bit [255:0] data, 
        input int be, 
        input int data_cnt,
        input bit soft_ctrl,
        input bit is_fabric_chi,
        input bit [511:0] flit
    );

    // rxdat：接收读数据 CompData
    import "DPI-C" function void dpi_chi_rxdat(
        input int mst_idx,
        input int txnid,
        input int opcode,
        input int dataid,   
        input bit [255:0] data, 
        input int be, 
        input int data_cnt,
        input bit soft_ctrl,
        input bit is_fabric_chi,
        input bit [511:0] flit
    );

    // rxsnp：for snp data
    import "DPI-C" function void dpi_chi_rxsnp(
        input int mst_idx,
        input int txnid,
        input longint addr,
        input longint req_time,
        input bit soft_ctrl,
        input bit is_fabric_chi,
        input bit [511:0] flit
    );

    // ---- Flit 域宽参数 ----
    parameter TXNID_W                                   = 12;
    parameter REQOPCODE_W                               = 4;
    parameter SIZE_W                                    = 4;
    parameter SECVEC_W                                  = 4;
    parameter ADDR_W                                    = 54;
    parameter RSPOPCODE_W                               = 3;
    parameter DBID_W                                    = 12;
    parameter DATOPCODE_W                               = 2;
    parameter DATAID_W                                  = 4;
    parameter BE_W                                      = 32;

    // ---- Flit 结构体定义 ----
    typedef struct packed {
        logic [TXNID_W-1:0]     txnid;
        logic [REQOPCODE_W-1:0] opcode;
        logic [ADDR_W-1:0]      addr;
        logic [SECVEC_W-1:0]    secvec;
        logic [SIZE_W-1:0]      size;
    } req_t;

    typedef struct packed {
        logic [TXNID_W-1:0]     txnid;
        logic [RSPOPCODE_W-1:0] opcode;
        logic [DBID_W-1:0]      dbid;
    } rsp_t;

    typedef struct packed {
        logic [TXNID_W-1:0]     txnid;
        logic [DATOPCODE_W-1:0] opcode;
        logic [DATAID_W-1:0]    dataid;
        logic [BE_W-1:0]        be;
        logic [255:0]           data;
        logic [5:0]             data_cnt;
    } dat_t;

    // ---- Flit 解析函数 ----
    function req_t req2dpi(input logic [REQ_W-1:0] req_flit);
        req_t req;
        req.txnid   = req_flit[`REQ_TXNID];
        req.opcode  = req_flit[`REQ_OPCODE];
        req.addr    = req_flit[`REQ_ADDR];
        req.secvec  = req_flit[`REQ_SECVEC];
        req.size    = req_flit[`REQ_SIZE];
        return req;
    endfunction

    function rsp_t rsp2dpi(input logic [RXRSP_W-1:0] rsp_flit);
        rsp_t rsp;
        rsp.txnid   = rsp_flit[`RSP_TXNID];
        rsp.opcode  = rsp_flit[`RSP_OPCODE];
        rsp.dbid    = rsp_flit[`RSP_DBID];
        return rsp;
    endfunction

    function dat_t txdat2dpi(input logic [TXDAT_W-1:0] dat_flit);
        dat_t dat;
        dat.txnid       = dat_flit[`DAT_TXNID];
        dat.opcode      = dat_flit[`DAT_OPCODE];
        dat.dataid      = dat_flit[`DAT_DATAID];
        dat.be          = dat_flit[`DAT_BE];
        dat.data        = dat_flit[`DAT_DATA];
        dat.data_cnt    = dat_flit[`DAT_DATA_CNT];
        return dat;
    endfunction

    function dat_t rxdat2dpi(input logic [RXDAT_W-1:0] dat_flit);
        dat_t dat;
        dat.txnid       = dat_flit[`DAT_TXNID];
        dat.opcode      = dat_flit[`DAT_OPCODE];
        dat.dataid      = dat_flit[`DAT_DATAID];
        dat.be          = dat_flit[`DAT_BE];
        dat.data        = dat_flit[`DAT_DATA];
        dat.data_cnt    = dat_flit[`DAT_DATA_CNT];
        return dat;
    endfunction

    // ---- 中间信号 ----
    req_t txreq[REQ_CHNS-1:0];
    rsp_t rxrsp[RXRSP_CHNS-1:0];
    dat_t txdat[TXDAT_CHNS-1:0];
    dat_t rxdat[RXDAT_CHNS-1:0];
    snp_t rxsnp[RXSNP_CHNS-1:0];

    // ---- txreq 解析 ----
    always_comb begin
        txreq = '0;
        if (txreq_flitv) begin
            txreq = req2dpi(txreq_flit);
        end
    end

    // ---- rxrsp 解析 & DPI调用 ----
    for (genvar i = 0; i < RXRSP_CHNS; i++) begin
        always_comb begin
            rxrsp[i] = '0;
            if (rxrsp_flitv[i]) begin
                rxrsp[i] = rsp2dpi(rxrsp_flit[i]);
            end
        end

        always_ff @(posedge clk) begin
            if (rxrsp_flitv[i]) begin
                dpi_chi_rxrsp_dbid(
                    MST_IDX,
                    rxrsp[i].txnid,
                    rxrsp[i].opcode,
                    rxrsp[i].dbid,
                    soft_ctrl,
                    IS_FABRIC_CHI,
                    512'(rxrsp_flit[i])
                );
            end
        end
    end

    // ---- txdat 解析 & DPI调用（NCBWrData，txnid实际为dbid） ----
    for (genvar i = 0; i < TXDAT_CHNS; i++) begin
        always_comb begin
            txdat[i] = '0;
            if (txdat_flitv[i]) begin
                txdat[i] = txdat2dpi(txdat_flit[i]);
            end
        end

        always_ff @(posedge clk) begin
            if (txdat_flitv[i]) begin
                dpi_chi_txdat(
                    MST_IDX,
                    txdat[i].txnid,   // CHI规定：NCBWrData.txnid = DBID
                    txdat[i].opcode,
                    txdat[i].dataid,
                    txdat[i].data,
                    txdat[i].be,
                    txdat[i].data_cnt,
                    soft_ctrl,
                    IS_FABRIC_CHI,
                    512'(txdat_flit[i])
                );
            end
        end
    end

    // ---- rxdat 解析 & DPI调用（CompData，txnid匹配原始txreq） ----
    for (genvar i = 0; i < RXDAT_CHNS; i++) begin
        always_comb begin
            rxdat[i] = '0;
            if (rxdat_flitv[i]) begin
                rxdat[i] = rxdat2dpi(rxdat_flit[i]);
            end
        end

        always_ff @(posedge clk) begin
            if (rxdat_flitv[i]) begin
                dpi_chi_rxdat(
                    MST_IDX,
                    rxdat[i].txnid,
                    rxdat[i].opcode,
                    rxdat[i].dataid,
                    rxdat[i].data,
                    rxdat[i].be,
                    rxdat[i].data_cnt,
                    soft_ctrl,
                    IS_FABRIC_CHI,
                    512'(rxdat_flit[i])
                );
            end
        end
    end

    for (genvar i = 0; i < REQ_CHNS; i++) begin
        // ---- txreq DPI调用（读写统一，opcode区分） ----
        always_ff @(posedge clk) begin
            if (txreq_flitv[i]) begin
                dpi_chi_txreq(
                    MST_IDX,
                    txreq[i].txnid, 
                    txreq[i].addr, 
                    txreq[i].size, 
                    txreq[i].opcode, 
                    txreq[i].secvec, 
                    $time,
                    IS_SM,
                    soft_ctrl,
                    IS_FABRIC_CHI,
                    512'(txreq_flit[i])
                );
            end
        end
    end

    if (IS_FABRIC_CHI) begin: SNP_BLOCK
        function snp_t snp2dpi(input logic [SNP_W-1:0] snp_flit);
            snp_t snp;
            snp.txnid   = snp_flit[`SNP_TXNID];
            snp.addr    = snp_flit[`SNP_ADDR];
            return snp;
        endfunction

        for (genvar i = 0; i < RXSNP_CHNS; i++) begin
            always_comb begin
                rxsnp[i] = '0;
                if (rxsnp_flitv[i]) begin
                    rxsnp[i] = snp2dpi(rxsnp_flit[i]);
                end
            end

            always_ff @(posedge clk) begin
                if (rxsnp_flitv[i]) begin
                    dpi_chi_rxsnp(
                        MST_IDX,
                        rxsnp[i].txnid,
                        rxsnp[i].addr,
                        $time,
                        soft_ctrl,
                        IS_FABRIC_CHI,
                        512'(rxsnp_flit[i])
                    );
                end
            end
        end
    end
    

endmodule