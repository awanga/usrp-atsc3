// frame_sync.v — frame synchronizer: chunk buffer, correlator and state machine
//
// Bit-exact port of lib/sync/frame_sync.cc (ATSC3_FIXED_POINT=ON). The
// input stream is cut into chunks by TLAST; each chunk is stored in an
// on-chip RAM and then handled exactly as one FrameSync::process() call
// (frame_sync_fsm.v describes the procedure; frame_correlator.v the
// correlation). The golden CLI hdl/sim/golden/frame_sync_gen takes the
// same chunking, so the pinned convention is simply: chunk boundaries are
// where TLAST is.
//
// Interface contract:
//   - s_axis: TDATA=ci16 {re, im}, TLAST on a chunk's last sample. A chunk
//     must be at least fft_size samples to be correlated at all (shorter
//     chunks are accepted and counted; the correlation loops simply find
//     nothing, as in the C++). s_axis_tready is high only while idle and
//     collecting; it drops from the cycle after TLAST is accepted until the
//     chunk's procedure and event have finished. The next chunk may then
//     start in the same cycle tready rises.
//   - bs_*: set_bootstrap_offset(). bs_ready is high while collecting with
//     no sample of the next chunk stored yet; bs_valid & bs_ready applies
//     bs_offset / bs_conf_q15 (a sample accepted that cycle is held off:
//     tready is low while bs_valid is high and the chunk is empty).
//   - m_axis: one FrameEvent word (status_words.vh) per FRAME_START event,
//     TLAST=1, held until accepted. The next chunk is not accepted until it
//     has been.
//   - cfg_*: sampled when TLAST is accepted and held for that chunk. fft_size outside 4..32768 or
//     above the RAM depth raises the sticky cfg_fft_size_invalid; a chunk
//     longer than the RAM raises the sticky chunk_overflow. Either way the
//     chunk is not processed and only the sample counter advances (fail
//     visibly; the C++ has neither limit).
//   - mon_*: the state machine's registers (state: 0 SEARCHING, 1
//     ACQUIRING, 2 LOCKED, 3 HOLDOVER), valid whenever s_axis_tready is
//     high (mon_sample_counter counts every sample of every chunk,
//     skipped ones included). mon_busy is high while a chunk is processed.
//   - Reset (synchronous, active-high): FrameSync::reset() plus clearing
//     the flags and any partial chunk.
//
// Throughput: sequential. One correlation is fft_size + ~170 cycles; an
// ACQUIRING or LOCKED chunk runs up to search_window+1 of them, so the
// input is held off for up to ~search_window * fft_size cycles per chunk
// (about 0.5M cycles at the 8K / 64 defaults). Overlapping the next chunk
// with the current one is throughput work.
//
// AXI4-S: TDATA=ci16 TVALID TREADY TLAST(chunk) in; TDATA=FrameEvent
// (136 bits) TVALID TREADY TLAST out.

`include "axi4s_types.vh"
`include "status_words.vh"

module frame_sync #(
    parameter ADDR_W = 16  // chunk RAM depth 2^ADDR_W samples
) (
    input  wire        clk,
    input  wire        rst,  // synchronous, active-high

    input  wire [15:0] cfg_fft_size,
    input  wire [15:0] cfg_cp_length,
    input  wire [15:0] cfg_symbols_per_subframe,
    input  wire [15:0] cfg_subframes_per_frame,
    input  wire signed [15:0] cfg_threshold_q15,
    input  wire [15:0] cfg_lock_acquire_count,
    input  wire [15:0] cfg_lock_loss_count,
    input  wire [15:0] cfg_holdover_frames,
    input  wire [15:0] cfg_search_window,
    output reg         cfg_fft_size_invalid,
    output reg         chunk_overflow,

    input  wire        bs_valid,
    output wire        bs_ready,
    input  wire [47:0] bs_offset,
    input  wire [15:0] bs_conf_q15,

    `AXI4S_SLAVE(s_axis, `ATSC3_SAMPLE_WIDTH),
    `AXI4S_MASTER(m_axis, `FRAME_EVENT_WIDTH),

    output wire        mon_busy,
    output wire [1:0]  mon_state,
    output wire [31:0] mon_frame_number,
    output wire [47:0] mon_expected_boundary,
    output wire [15:0] mon_confidence,
    output wire [47:0] mon_samples_to_boundary,
    output wire [47:0] mon_sample_counter
);

    localparam DEPTH = 1 << ADDR_W;

    localparam [1:0] T_RX   = 2'd0,
                     T_PROC = 2'd1,
                     T_EMIT = 2'd2;

    reg [1:0]  top_state;
    reg [31:0] n;           // samples collected in the current chunk

    // Chunk RAM: written by the stream, read by the correlator.
    reg [31:0]        mem [0:DEPTH-1];
    reg [31:0]        rd_data;
    wire [ADDR_W-1:0] rd_addr;

    always @(posedge clk) begin
        rd_data <= mem[rd_addr];
    end

    wire at_start = (n == 32'd0);
    assign bs_ready = (top_state == T_RX) && at_start;
    wire bs_apply   = bs_valid && bs_ready;

    wire rx_ok = (top_state == T_RX) && !(bs_valid && at_start);
    wire fire  = s_axis_tvalid && rx_ok;

    always @* begin
        s_axis_tready = rx_ok;
    end

    wire [31:0] fft32 = {16'd0, cfg_fft_size};
    wire        fft_bad = (cfg_fft_size < 16'd4) || (fft32 > DEPTH);
    // 32768 is the largest value 16 bits hold, so no explicit upper bound.

    // The TLAST beat hands the chunk to the state machine on the same edge
    // it is stored, which is also when the configuration is sampled.
    wire        proc_start = fire && s_axis_tlast;
    wire [31:0] proc_n     = n + 32'd1;
    wire        proc_skip  = fft_bad || (proc_n > DEPTH);

    // FSM and correlator
    wire        proc_done;
    wire        corr_start, corr_done;
    wire [31:0] corr_offset;
    wire [15:0] corr_len;
    wire [15:0] corr_ratio;
    wire        ev_valid;
    wire [47:0] ev_sample_index;
    wire [31:0] ev_frame_number;
    wire [15:0] ev_confidence;

    frame_sync_fsm u_fsm (
        .clk(clk), .rst(rst),
        .cfg_fft_size(cfg_fft_size),
        .cfg_cp_length(cfg_cp_length),
        .cfg_symbols_per_subframe(cfg_symbols_per_subframe),
        .cfg_subframes_per_frame(cfg_subframes_per_frame),
        .cfg_threshold_q15(cfg_threshold_q15),
        .cfg_lock_acquire_count(cfg_lock_acquire_count),
        .cfg_lock_loss_count(cfg_lock_loss_count),
        .cfg_holdover_frames(cfg_holdover_frames),
        .cfg_search_window(cfg_search_window),
        .bs_apply(bs_apply), .bs_offset(bs_offset), .bs_conf(bs_conf_q15),
        .proc_start(proc_start), .proc_skip(proc_skip), .proc_n(proc_n),
        .proc_done(proc_done),
        .corr_start(corr_start), .corr_offset(corr_offset), .corr_len(corr_len),
        .corr_done(corr_done), .corr_ratio(corr_ratio),
        .ev_valid(ev_valid), .ev_sample_index(ev_sample_index),
        .ev_frame_number(ev_frame_number), .ev_confidence(ev_confidence),
        .mon_state(mon_state), .mon_frame_number(mon_frame_number),
        .mon_expected_boundary(mon_expected_boundary),
        .mon_confidence(mon_confidence),
        .mon_samples_to_boundary(mon_samples_to_boundary),
        .mon_sample_counter(mon_sample_counter)
    );

    wire corr_busy;

    frame_correlator #(.ADDR_W(ADDR_W)) u_corr (
        .clk(clk), .rst(rst),
        .start(corr_start),
        .base(corr_offset[ADDR_W-1:0]),
        .len(corr_len),
        .busy(corr_busy), .done(corr_done), .ratio(corr_ratio),
        .rd_addr(rd_addr), .rd_data(rd_data)
    );

    assign mon_busy = (top_state != T_RX);

    wire unused_ok = &{1'b0, corr_busy, corr_offset[31:ADDR_W], 1'b0};

    always @(posedge clk) begin
        if (rst) begin
            top_state            <= T_RX;
            n                    <= 32'd0;
            cfg_fft_size_invalid <= 1'b0;
            chunk_overflow       <= 1'b0;
            m_axis_tdata         <= {`FRAME_EVENT_WIDTH{1'b0}};
            m_axis_tvalid        <= 1'b0;
            m_axis_tlast         <= 1'b0;
        end else begin
            if (m_axis_tvalid && m_axis_tready)
                m_axis_tvalid <= 1'b0;

            if (ev_valid) begin
                m_axis_tdata                                                            <= {`FRAME_EVENT_WIDTH{1'b0}};
                m_axis_tdata[`FRAME_EVENT_TYPE_HI:`FRAME_EVENT_TYPE_LO]                 <= `FRAME_EVENT_TYPE_FRAME_START;
                m_axis_tdata[`FRAME_EVENT_SAMPLE_INDEX_HI:`FRAME_EVENT_SAMPLE_INDEX_LO] <= ev_sample_index;
                m_axis_tdata[`FRAME_EVENT_FRAME_NUMBER_HI:`FRAME_EVENT_FRAME_NUMBER_LO] <= ev_frame_number;
                m_axis_tdata[`FRAME_EVENT_CONFIDENCE_HI:`FRAME_EVENT_CONFIDENCE_LO]     <= ev_confidence;
                m_axis_tvalid                                                           <= 1'b1;
                m_axis_tlast                                                            <= 1'b1;
            end

            case (top_state)
                T_RX: begin
                    if (fire) begin
                        if (n < DEPTH)
                            mem[n[ADDR_W-1:0]] <= s_axis_tdata;
                        else
                            chunk_overflow <= 1'b1;
                        n <= proc_n;
                        if (s_axis_tlast) begin
                            if (fft_bad)
                                cfg_fft_size_invalid <= 1'b1;
                            top_state <= T_PROC;
                        end
                    end
                end

                T_PROC: begin
                    if (proc_done) begin
                        n         <= 32'd0;
                        top_state <= T_EMIT;
                    end
                end

                default: begin  // T_EMIT: the event word, if any, is out
                    if (!m_axis_tvalid && !ev_valid)
                        top_state <= T_RX;
                end
            endcase
        end
    end

endmodule
