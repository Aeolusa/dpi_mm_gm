  
module mm_gm_top #(
    parameter REQ_W                                     = 1,
    parameter TXRSP_CHNS                                = 1,
    parameter TXRSP_W                                   = 1,
    parameter RXRSP_CHNS                                = 1,
    parameter RXRSP_W                                   = 1, 
    parameter TXDAT_CHNS                                = 1,
    parameter TXDAT_W                                   = 1,
    parameter RXDAT_CHNS                                = 1,
    parameter RXDAT_W                                   = 1 
) (
    input bit                                           clk,
    input bit                                           rstn,

    input logic [REQ_W-1:0]                             txreq_flit,
    input logic                                         txreq_flitv,
    input logic [TXRSP_CHNS-1:0][TXRSP_W-1:0]           txrsp_flit,
    input logic [TXRSP_CHNS-1:0]                        txrsp_flitv,
    input logic [RXRSP_CHNS-1:0][RXRSP_W-1:0]           rxrsp_flit,
    input logic [RXRSP_CHNS-1:0]                        rxrsp_flitv,
    input logic [TXDAT_CHNS-1:0][TXDAT_W-1:0]           txdat_flit,
    input logic [TXDAT_CHNS-1:0]                        txdat_flitv,
    input logic [RXDAT_CHNS-1:0][RXDAT_W-1:0]           rxdat_flit,
    input logic [RXDAT_CHNS-1:0]                        rxdat_flitv           
);

    import "DPI-C" function void dpi_chi_txreq_write(
        input int txnid,
        input longint addr, 
        input int size, 
        input int opcode,
        input int secvec,
        input longint req_time
    );

    import "DPI-C" function void dpi_chi_rxrsp_dbid(
        input int txnid, 
        input int opcode,
        input int dbid
    );

    // ==========================================
    // 重构后的高效 Data 传输接口：
    // ==========================================

    // 发送写数据 flit (原生 256-bit data + 32-bit byte_en)
    import "DPI-C" function void dpi_chi_txdat(
        input int txnid,
        input int opcode,
        input int dataid,   
        input bit [255:0] data, 
        input int be, 
        input int data_cnt
    );

    import "DPI-C" function void dpi_chi_rxdat(
        input int txnid,
        input int opcode,
        input int dataid,   
        input bit [255:0] data, 
        input int be, 
        input int data_cnt
    );

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

    typedef struct packed {
        logic [TXNID_W-1:0]     txnid;
        logic [OPCODE_W-1:0]    opcode;
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

    function req_t req2dpi(input logic [REQ_W-1:0] req_flit);
        req_t req;
        req.txn_id  = req_flit[`REQ_TXNID];
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

    function dat_t dat2dpi(input logic [TXDAT_W-1:0] dat_flit);
        dat_t dat;
        dat.txnid       = dat_flit[`DAT_TXNID];
        dat.opcode      = dat_flit[`DAT_OPCODE];
        dat.dataid      = dat_flit[`DAT_DATAID];
        dat.be          = dat_flit[`DAT_BE];
        dat.data        = dat_flit[`DAT_DATA];
        dat.data_cnt    = dat_flit[`DAT_DATA_CNT];
        return dat;
    endfunction

    req_t txreq;
    rsp_t rxrsp[RXRSP_CHNS-1:0];
    dat_t txdat[TXDAT_CHNS-1:0];
    dat_t rxdat[RXDAT_CHNS-1:0];

    always_comb begin
        txreq = '0;
        if (txreq_flitv) begin
            txreq = req2dpi(txreq_flit);
        end
    end

    for (genvar i = 0; i < RXRSP_CHNS; i++) begin
        always_comb begin
            rxrsp[i] = '0;
            if (rxrsp_flitv[i]) begin
                rxrsp[i] = rsp2dpi(rxrsp_flit[i]);
            end
        end

        always_ff @(posedge clk) begin
            if (rxrsp_flitv[i]) begin
                dpi_chi_rxrsp_dbid(rxrsp[i].txnid, rxrsp[i].opcode, rxrsp[i].dbid);
            end
        end
    end

    for (genvar i = 0; i < TXDAT_CHNS; i++) begin
        always_comb begin
            txdat[i] = '0;
            if (txdat_flitv[i]) begin
                txdat[i] = dat2dpi(txdat_flit[i]);
            end
        end

        always_ff @(posedge clk) begin
            if (txdat_flitv[i]) begin
                dpi_chi_txdat(txdat[i].txnid,
                              txdat[i].opcode,
                              txdat[i].dataid,
                              txdat[i].data,
                              txdat[i].be,
                              txdat[i].data_cnt);
            end
        end
    end

    for (genvar i = 0; i < RXDAT_CHNS; i++) begin
        always_comb begin
            rxdat[i] = '0;
            if (rxdat_flitv[i]) begin
                rxdat[i] = dat2dpi(rxdat_flit[i]);
            end
        end

        always_ff @(posedge clk) begin
            if (rxdat_flitv[i]) begin
                dpi_chi_rxdat(rxdat[i].txnid,
                              rxdat[i].opcode,
                              rxdat[i].dataid,
                              rxdat[i].data,
                              rxdat[i].be,
                              rxdat[i].data_cnt);
            end
        end
    end

    always_ff @(posedge clk) begin
        if (txreq_flitv) begin
            dpi_chi_txreq_write(txreq.txnid, 
                                txreq.addr, 
                                txreq.size, 
                                txreq.opcode, 
                                txreq.secvec, 
                                $time)
        end
    end

endmodule