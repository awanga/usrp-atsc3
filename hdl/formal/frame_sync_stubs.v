// frame_sync_stubs.v — formal-only stand-ins for the state machine and the
// correlator inside frame_sync.v
//
// frame_sync_formal.v checks the chunk buffer and AXI4-S framing in
// frame_sync.v. Each block it instantiates is proved separately
// (frame_sync_fsm.sby, frame_correlator.sby), so here they are replaced by
// modules with the same ports that obey the contracts those proofs
// establish and are otherwise arbitrary:
//   state machine: one proc_done per proc_start, at a solver-chosen time
//   at least one cycle later; at most one event, only while a chunk is in
//   flight; all status outputs arbitrary.
//   correlator: busy / done / ratio arbitrary, rd_addr arbitrary.
//
// Verification-only. Not synthesized, not part of rtl/.

module frame_sync_fsm (
    input  wire        clk,
    input  wire        rst,
    input  wire [15:0] cfg_fft_size,
    input  wire [15:0] cfg_cp_length,
    input  wire [15:0] cfg_symbols_per_subframe,
    input  wire [15:0] cfg_subframes_per_frame,
    input  wire signed [15:0] cfg_threshold_q15,
    input  wire [15:0] cfg_lock_acquire_count,
    input  wire [15:0] cfg_lock_loss_count,
    input  wire [15:0] cfg_holdover_frames,
    input  wire [15:0] cfg_search_window,
    input  wire        bs_apply,
    input  wire [47:0] bs_offset,
    input  wire [15:0] bs_conf,
    input  wire        proc_start,
    input  wire        proc_skip,
    input  wire [31:0] proc_n,
    output reg         proc_done,
    output reg         corr_start,
    output reg  [31:0] corr_offset,
    output wire [15:0] corr_len,
    input  wire        corr_done,
    input  wire [15:0] corr_ratio,
    output reg         ev_valid,
    output reg  [47:0] ev_sample_index,
    output reg  [31:0] ev_frame_number,
    output reg  [15:0] ev_confidence,
    output wire [1:0]  mon_state,
    output wire [31:0] mon_frame_number,
    output wire [47:0] mon_expected_boundary,
    output wire [15:0] mon_confidence,
    output wire [47:0] mon_samples_to_boundary,
    output wire [47:0] mon_sample_counter
);
    reg        busy;
    reg        ev_sent;
    reg        go;        // free: finish the chunk
    reg        ev_go;     // free: emit the event
    reg [15:0] free16;
    reg [47:0] free48;
    reg [31:0] free32;
    reg [1:0]  free2;

    assign corr_len                = free16;
    assign mon_state               = free2;
    assign mon_frame_number        = free32;
    assign mon_expected_boundary   = free48;
    assign mon_confidence          = free16;
    assign mon_samples_to_boundary = free48;
    assign mon_sample_counter      = free48;

    wire unused = &{1'b0, cfg_fft_size, cfg_cp_length, cfg_symbols_per_subframe,
                    cfg_subframes_per_frame, cfg_threshold_q15, cfg_lock_acquire_count,
                    cfg_lock_loss_count, cfg_holdover_frames, cfg_search_window,
                    bs_apply, bs_offset, bs_conf, proc_skip, proc_n, corr_done,
                    corr_ratio, 1'b0};

    always @(posedge clk) begin
        if (rst) begin
            busy      <= 1'b0;
            ev_sent   <= 1'b0;
            proc_done <= 1'b0;
            ev_valid  <= 1'b0;
        end else begin
            proc_done  <= 1'b0;
            ev_valid   <= 1'b0;
            corr_start <= 1'b0;
            if (!busy) begin
                if (proc_start) begin
                    busy    <= 1'b1;
                    ev_sent <= 1'b0;
                end
            end else begin
                if (ev_go && !ev_sent) begin
                    ev_valid        <= 1'b1;
                    ev_sent         <= 1'b1;
                    ev_sample_index <= free48;
                    ev_frame_number <= free32;
                    ev_confidence   <= free16;
                end
                if (go) begin
                    busy      <= 1'b0;
                    proc_done <= 1'b1;
                end
            end
        end
    end

    always @(*) corr_offset = free32;
endmodule

module frame_correlator #(
    parameter ADDR_W = 16
) (
    input  wire              clk,
    input  wire              rst,
    input  wire              start,
    input  wire [ADDR_W-1:0] base,
    input  wire [15:0]       len,
    output reg               busy,
    output reg               done,
    output reg  [15:0]       ratio,
    output reg  [ADDR_W-1:0] rd_addr,
    input  wire [31:0]       rd_data
);
    wire unused = &{1'b0, clk, rst, start, base, len, rd_data, 1'b0};
    always @(*) begin
        busy    = 1'b0;
        done    = 1'b0;
        ratio   = 16'd0;
        rd_addr = {ADDR_W{1'b0}};
    end
endmodule
