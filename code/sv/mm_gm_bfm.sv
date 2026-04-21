


module mm_gm_bfm (
    
);

    

    // file: refmodel_dpi.sv
    import "DPI-C" function void refmodel_init(int num_masters, int strict_mode);
    import "DPI-C" function void refmodel_write(
        int master_id, longint addr, int size, int burst_len,
        string data_hex, string byte_en_hex,
        longint req_time, longint resp_time
    );
    import "DPI-C" function int refmodel_read_check(
        int master_id, longint addr, int size, int burst_len,
        string resp_data_hex,
        longint req_time, longint resp_time
    );
    import "DPI-C" function void refmodel_finish();



endmodule