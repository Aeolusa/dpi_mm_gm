`define REQ_TXNID                                           26:15
`define REQ_OPCODE                                          30:27
`define REQ_SIZE                                            34:31
`define REQ_SECVEC                                          38:35
`define REQ_ADDR                                            92:39   
`define RSP_TXNID                                           26:15
`define RSP_OPCODE                                          29:27
`define RSP_DBID                                            46:35    
`define DAT_TXNID                                           26:15
`define DAT_OPCODE                                          28:27
`define DAT_DATAID                                          34:31
`define DAT_BE                                              322:291
`define DAT_DATA                                            290:35
`define DAT_DATA_CNT                                        325:324         

module mm_gm_top #(
    parameter NID_WIDTH                                 = 11,
    parameter SM_NUMS                                   = 8,
    parameter SM_REQ_W                                  = 1, 
    parameter SM_TXDAT_W                                = 1,
    parameter SM_RXDAT_W                                = 1,
    parameter SM_TXRSP_W                                = 1,
    parameter SM_RXRSP_W                                = 1,
    parameter SM_REQCHN_NUMS                            = 1,
    parameter SM_TXRSPCHN_NUMS                          = 1,
    parameter SM_RXRSPCHN_NUMS                          = 1,
    parameter SM_TXDATCHN_NUMS                          = 1,
    parameter SM_RXDATCHN_NUMS                          = 1,
    parameter HST_REQ_W                                 = 1,
    parameter HST_TXDAT_W                               = 1,
    parameter HST_RXDAT_W                               = 1,
    parameter HST_TXRSP_W                               = 1,
    parameter HST_RXRSP_W                               = 1,
    parameter HST_REQCHN_NUMS                           = 1,
    parameter HST_TXRSPCHN_NUMS                         = 1,
    parameter HST_RXRSPCHN_NUMS                         = 1,
    parameter HST_TXDATCHN_NUMS                         = 1,
    parameter HST_RXDATCHN_NUMS                         = 1,
    parameter TS_REQ_W                                  = 1,
    parameter TS_TXDAT_W                                = 1,
    parameter TS_RXDAT_W                                = 1,
    parameter TS_TXRSP_W                                = 1,
    parameter TS_RXRSP_W                                = 1,
    parameter TS_REQCHN_NUMS                            = 1,
    parameter TS_TXRSPCHN_NUMS                          = 1,
    parameter TS_RXRSPCHN_NUMS                          = 1,
    parameter TS_TXDATCHN_NUMS                          = 1,
    parameter TS_RXDATCHN_NUMS                          = 1,
    parameter BLIT_REQ_W                                = 1,
    parameter BLIT_TXDAT_W                              = 1,
    parameter BLIT_RXDAT_W                              = 1,
    parameter BLIT_TXRSP_W                              = 1,
    parameter BLIT_RXRSP_W                              = 1,
    parameter BLIT_REQCHN_NUMS                          = 1,
    parameter BLIT_TXRSPCHN_NUMS                        = 1,
    parameter BLIT_RXRSPCHN_NUMS                        = 1,
    parameter BLIT_TXDATCHN_NUMS                        = 1,
    parameter BLIT_RXDATCHN_NUMS                        = 1
) (
    input bit                                           sm_clk,
    input bit                                           host_clk,
    input bit                                           ts_clk,
    input bit                                           blit_clk,
    input bit                                           sm_rstn,
    input bit                                           host_rstn,
    input bit                                           ts_rstn,
    input bit                                           blit_rstn,
    // SM
    input logic [SM_REQ_W-1:0]                          sm_txreq_flit,
    input logic                                         sm_txreq_flitv,
    input logic [SM_TXRSP_CHNS-1:0][SM_TXRSP_W-1:0]     sm_txrsp_flit,
    input logic [SM_TXRSP_CHNS-1:0]                     sm_txrsp_flitv,
    input logic [SM_RXRSP_CHNS-1:0][SM_RXRSP_W-1:0]     sm_rxrsp_flit,
    input logic [SM_RXRSP_CHNS-1:0]                     sm_rxrsp_flitv,
    input logic [SM_TXDAT_CHNS-1:0][SM_TXDAT_W-1:0]     sm_txdat_flit,
    input logic [SM_TXDAT_CHNS-1:0]                     sm_txdat_flitv,
    input logic [SM_RXDAT_CHNS-1:0][SM_RXDAT_W-1:0]     sm_rxdat_flit,
    input logic [SM_RXDAT_CHNS-1:0]                     sm_rxdat_flitv,
    // HOST          
    input logic [HST_REQ_W-1:0]                         host_txreq_flit,
    input logic                                         host_txreq_flitv,
    input logic [HST_TXRSP_CHNS-1:0][HST_TXRSP_W-1:0]   host_txrsp_flit,
    input logic [HST_TXRSP_CHNS-1:0]                    host_txrsp_flitv,
    input logic [HST_RXRSP_CHNS-1:0][HST_RXRSP_W-1:0]   host_rxrsp_flit,
    input logic [HST_RXRSP_CHNS-1:0]                    host_rxrsp_flitv,
    input logic [HST_TXDAT_CHNS-1:0][HST_TXDAT_W-1:0]   host_txdat_flit,
    input logic [HST_TXDAT_CHNS-1:0]                    host_txdat_flitv,
    input logic [HST_RXDAT_CHNS-1:0][HST_RXDAT_W-1:0]   host_rxdat_flit,
    input logic [HST_RXDAT_CHNS-1:0]                     host_rxdat_flitv,
    // TS          
    input logic [TS_REQ_W-1:0]                          ts_txreq_flit,
    input logic                                         ts_txreq_flitv,
    input logic [TS_TXRSP_CHNS-1:0][TS_TXRSP_W-1:0]     ts_txrsp_flit,
    input logic [TS_TXRSP_CHNS-1:0]                     ts_txrsp_flitv,
    input logic [TS_RXRSP_CHNS-1:0][TS_RXRSP_W-1:0]     ts_rxrsp_flit,
    input logic [TS_RXRSP_CHNS-1:0]                     ts_rxrsp_flitv,
    input logic [TS_TXDAT_CHNS-1:0][TS_TXDAT_W-1:0]     ts_txdat_flit,
    input logic [TS_TXDAT_CHNS-1:0]                     ts_txdat_flitv,
    input logic [TS_RXDAT_CHNS-1:0][TS_RXDAT_W-1:0]   ts_rxdat_flit,
    input logic [TS_RXDAT_CHNS-1:0]                   ts_rxdat_flitv,
    // BLIT          
    input logic [BLIT_REQ_W-1:0]                        blit_txreq_flit,
    input logic                                         blit_txreq_flitv,
    input logic [BLIT_TXRSP_CHNS-1:0][BLIT_TXRSP_W-1:0] blit_txrsp_flit,
    input logic [BLIT_TXRSP_CHNS-1:0]                   blit_txrsp_flitv,
    input logic [BLIT_RXRSP_CHNS-1:0][BLIT_RXRSP_W-1:0] blit_rxrsp_flit,
    input logic [BLIT_RXRSP_CHNS-1:0]                   blit_rxrsp_flitv,
    input logic [BLIT_TXDAT_CHNS-1:0][BLIT_TXDAT_W-1:0] blit_txdat_flit,
    input logic [BLIT_TXDAT_CHNS-1:0]                   blit_txdat_flitv,
    input logic [BLIT_RXDAT_CHNS-1:0][BLIT_RXDAT_W-1:0] blit_rxdat_flit,
    input logic [BLIT_RXDAT_CHNS-1:0]                   blit_rxdat_flitv
);

    // ---- 初始化、请求、响应的接口均无变化 ----
    import "DPI-C" function void refmodel_init(input int strict_mode);

    bit initial_m1;
    bit initial_m2

    always_ff @(posedge clk) begin
        if (!sm_rstn) begin
            initial_m1 <= 1'b1;
        end else begin
            initial_m1 <= 1'b0;
        end
    end

    always_ff @(posedge clk) begin
        initial_m2 <= initial_m1;
    end

    always_ff @(posedge clk) begin
        if (!initial_m1 && initial_m2) begin
            refmodel_init();
        end
    end

    // SM BFM inst (MST_IDX = genvar idx, 0..SM_NUMS-1)
    for (genvar idx = 0; idx < SM_NUMS; idx++) begin : SM_BFM
        mm_gm_bfm #(
            .MST_IDX                                (idx),
            .REQ_W                                  (SM_REQ_W),
            .TXRSP_CHNS                             (SM_TXRSP_CHNS),
            .TXRSP_W                                (SM_TXRSP_W),
            .RXRSP_CHNS                             (SM_RXRSP_CHNS),
            .RXRSP_W                                (SM_RXRSP_W),
            .TXDAT_CHNS                             (SM_TXDAT_CHNS),
            .TXDAT_W                                (SM_TXDAT_W),
            .RXDAT_CHNS                             (SM_RXDAT_CHNS),
            .RXDAT_W                                (SM_RXDAT_W)
        ) u_sm_bfm (
            .clk                                    (sm_clk),
            .rstn                                   (sm_rstn),
            .txreq_flit                             (sm_txreq_flit),
            .txreq_flitv                            (sm_txreq_flitv),
            .txrsp_flit                             (sm_txrsp_flit),
            .txrsp_flitv                            (sm_txrsp_flitv),
            .rxrsp_flit                             (sm_rxrsp_flit),
            .rxrsp_flitv                            (sm_rxrsp_flitv),
            .txdat_flit                             (sm_txdat_flit),
            .txdat_flitv                            (sm_txdat_flitv),
            .rxdat_flit                             (sm_rxdat_flit),
            .rxdat_flitv                            (sm_rxdat_flitv)
        );
    end

    // HOST BFM inst (MST_IDX = SM_NUMS, e.g. 8)
    mm_gm_bfm #(
        .MST_IDX                                (SM_NUMS),
        .REQ_W                                  (HST_REQ_W),
        .TXRSP_CHNS                             (HST_TXRSP_CHNS),
        .TXRSP_W                                (HST_TXRSP_W),
        .RXRSP_CHNS                             (HST_RXRSP_CHNS),
        .RXRSP_W                                (HST_RXRSP_W),
        .TXDAT_CHNS                             (HST_TXDAT_CHNS),
        .TXDAT_W                                (HST_TXDAT_W),
        .RXDAT_CHNS                             (HST_RXDAT_CHNS),
        .RXDAT_W                                (HST_RXDAT_W)
    ) u_hst_bfm (
        .clk                                    (hst_clk),
        .rstn                                   (hst_rstn),
        .txreq_flit                             (hst_txreq_flit),
        .txreq_flitv                            (hst_txreq_flitv),
        .txrsp_flit                             (hst_txrsp_flit),
        .txrsp_flitv                            (hst_txrsp_flitv),
        .rxrsp_flit                             (hst_rxrsp_flit),
        .rxrsp_flitv                            (hst_rxrsp_flitv),
        .txdat_flit                             (hst_txdat_flit),
        .txdat_flitv                            (hst_txdat_flitv),
        .rxdat_flit                             (hst_rxdat_flit),
        .rxdat_flitv                            (hst_rxdat_flitv)
    );

    // TS BFM inst (MST_IDX = SM_NUMS+1, e.g. 9)
    mm_gm_bfm #(
        .MST_IDX                                (SM_NUMS + 1),
        .REQ_W                                  (TS_REQ_W),
        .TXRSP_CHNS                             (TS_TXRSP_CHNS),
        .TXRSP_W                                (TS_TXRSP_W),
        .RXRSP_CHNS                             (TS_RXRSP_CHNS),
        .RXRSP_W                                (TS_RXRSP_W),
        .TXDAT_CHNS                             (TS_TXDAT_CHNS),
        .TXDAT_W                                (TS_TXDAT_W),
        .RXDAT_CHNS                             (TS_RXDAT_CHNS),
        .RXDAT_W                                (TS_RXDAT_W)
    ) u_ts_bfm (
        .clk                                    (ts_clk),
        .rstn                                   (ts_rstn),
        .txreq_flit                             (ts_txreq_flit),
        .txreq_flitv                            (ts_txreq_flitv),
        .txrsp_flit                             (ts_txrsp_flit),
        .txrsp_flitv                            (ts_txrsp_flitv),
        .rxrsp_flit                             (ts_rxrsp_flit),
        .rxrsp_flitv                            (ts_rxrsp_flitv),
        .txdat_flit                             (ts_txdat_flit),
        .txdat_flitv                            (ts_txdat_flitv),
        .rxdat_flit                             (ts_rxdat_flit),
        .rxdat_flitv                            (ts_rxdat_flitv)
    );

    // BLIT BFM inst (MST_IDX = SM_NUMS+2, e.g. 10)
    mm_gm_bfm #(
        .MST_IDX                                (SM_NUMS + 2),
        .REQ_W                                  (BLIT_REQ_W),
        .TXRSP_CHNS                             (BLIT_TXRSP_CHNS),
        .TXRSP_W                                (BLIT_TXRSP_W),
        .RXRSP_CHNS                             (BLIT_RXRSP_CHNS),
        .RXRSP_W                                (BLIT_RXRSP_W),
        .TXDAT_CHNS                             (BLIT_TXDAT_CHNS),
        .TXDAT_W                                (BLIT_TXDAT_W),
        .RXDAT_CHNS                             (BLIT_RXDAT_CHNS),
        .RXDAT_W                                (BLIT_RXDAT_W)
    ) u_blit_bfm (
        .clk                                    (blit_clk),
        .rstn                                   (blit_rstn),
        .txreq_flit                             (blit_txreq_flit),
        .txreq_flitv                            (blit_txreq_flitv),
        .txrsp_flit                             (blit_txrsp_flit),
        .txrsp_flitv                            (blit_txrsp_flitv),
        .rxrsp_flit                             (blit_rxrsp_flit),
        .rxrsp_flitv                            (blit_rxrsp_flitv),
        .txdat_flit                             (blit_txdat_flit),
        .txdat_flitv                            (blit_txdat_flitv),
        .rxdat_flit                             (blit_rxdat_flit),
        .rxdat_flitv                            (blit_rxdat_flitv)
    );

endmodule