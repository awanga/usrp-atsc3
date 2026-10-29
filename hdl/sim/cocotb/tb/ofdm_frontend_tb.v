// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// ofdm_frontend_tb.v — integration top: freq_correction -> cp_removal ->
// fft_engine -> pilot_extractor, with frame_sync tapping the frequency-
// corrected stream
//
// Testbench-only wiring for test_ofdm_frontend_tb.py: the four blocks
// connected as in the receiver, one clock domain. The FFT output link is
// also brought out as a tap (fft_*) so the bench can check every FFT bin,
// not just the pilots. The symbol counter feeding the pilot extractor's
// TUSER stands in for frame sync's symbol index. The beat and symbol
// counters are testbench instrumentation for the status checks.
//
// frame_sync sees the same corrected stream through a fork (a beat moves
// only when both the cp_removal path and frame_sync accept it, so frame_sync
// processing a chunk stalls the chain). The bench cuts the stream into
// chunks of cfg_chunk_len samples (TLAST on every cfg_chunk_len-th beat),
// as the model's process() calls are cut in test_ofdm_frontend_tb.py.

module ofdm_frontend_tb #(
    parameter TWIDDLE_HEX = "fft_twiddles.hex",
    parameter CP_ROM_HEX  = "pilot_cp_positions.hex",
    parameter FS_ADDR_W   = 15
) (
    input  wire        clk,
    input  wire        rst,
    input  wire [31:0] cfg_phase_inc,
    input  wire [17:0] cfg_fft_size,
    input  wire [12:0] cfg_cp_fraction_numerator,
    input  wire [3:0]  cfg_pilot_pattern,
    output wire        cp_fft_size_invalid,
    output wire        cp_numerator_clamped,
    output wire        fft_size_invalid,
    output wire        pe_fft_size_invalid,
    output wire        pe_pattern_invalid,
    output wire        pe_num_active_invalid,
    output wire        pe_framing_error,

    input  wire [15:0] cfg_cp_length,     // CP samples, for frame_sync
    input  wire [15:0] cfg_chunk_len,     // frame_sync chunk size
    output wire        fs_cfg_invalid,
    output wire        fs_chunk_overflow,
    output wire        fs_mon_busy,
    output wire [1:0]  fs_mon_state,
    output wire [31:0] fs_mon_frame_number,
    output wire [47:0] fs_mon_expected_boundary,
    output wire [15:0] fs_mon_confidence,
    output wire [47:0] fs_mon_sample_counter,
    output wire [135:0] fs_ev_tdata,
    output wire        fs_ev_tvalid,
    input  wire        fs_ev_tready,
    output wire        fs_ev_tlast,

    input  wire [31:0] s_axis_tdata,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,

    output wire [95:0] m_axis_tdata,
    output wire        m_axis_tvalid,
    input  wire        m_axis_tready,
    output wire        m_axis_tlast,

    output wire [31:0] fft_tdata,       // tap on the fft -> pilot link
    output wire        fft_fire,
    output wire        fft_tlast,

    output reg  [31:0] fc_out_beats,    // beats freq_correction handed on
    output reg  [31:0] cp_out_beats,    // beats cp_removal handed to fft_engine
    output reg  [31:0] cp_out_symbols,  // TLASTs on that link
    output reg  [31:0] fft_out_symbols  // TLASTs into pilot_extractor
);

    wire [31:0] fc_tdata, cp_tdata, fft_tdata_w;
    wire        fc_tvalid, fc_tready, fc_tlast;
    wire        fc_cp_tready;      // cp_removal's ready, before the fork
    wire        fs_tready;
    wire        fc_cp_tvalid;
    wire        fc_fs_tvalid;
    wire        cp_tvalid, cp_tready, cp_tlast;
    wire        fft_tvalid, fft_tready;

    freq_correction u_fc (
        .clk           (clk),
        .rst           (rst),
        .cfg_phase_inc (cfg_phase_inc),
        .phase_reset   (1'b0),
        .s_axis_tdata  (s_axis_tdata),
        .s_axis_tvalid (s_axis_tvalid),
        .s_axis_tready (s_axis_tready),
        .s_axis_tlast  (s_axis_tlast),
        .m_axis_tdata  (fc_tdata),
        .m_axis_tvalid (fc_tvalid),
        .m_axis_tready (fc_tready),
        .m_axis_tlast  (fc_tlast)
    );

    cp_removal u_cp (
        .clk                       (clk),
        .rst                       (rst),
        .cfg_fft_size              (cfg_fft_size),
        .cfg_cp_fraction_numerator (cfg_cp_fraction_numerator),
        .cfg_fft_size_invalid      (cp_fft_size_invalid),
        .cfg_cp_numerator_clamped  (cp_numerator_clamped),
        .s_axis_tdata              (fc_tdata),
        .s_axis_tvalid             (fc_cp_tvalid),
        .s_axis_tready             (fc_cp_tready),
        .s_axis_tlast              (fc_tlast),
        .m_axis_tdata              (cp_tdata),
        .m_axis_tvalid             (cp_tvalid),
        .m_axis_tready             (cp_tready),
        .m_axis_tlast              (cp_tlast)
    );

    fft_engine #(.TWIDDLE_HEX(TWIDDLE_HEX)) u_fft (
        .clk                  (clk),
        .rst                  (rst),
        .cfg_fft_size         (cfg_fft_size),
        .cfg_fft_size_invalid (fft_size_invalid),
        .s_axis_tdata         (cp_tdata),
        .s_axis_tvalid        (cp_tvalid),
        .s_axis_tready        (cp_tready),
        .s_axis_tlast         (cp_tlast),
        .m_axis_tdata         (fft_tdata_w),
        .m_axis_tvalid        (fft_tvalid),
        .m_axis_tready        (fft_tready),
        .m_axis_tlast         (fft_tlast)
    );

    // Fork: a corrected beat is taken by both consumers on the same edge.
    assign fc_tready    = fc_cp_tready && fs_tready;
    assign fc_cp_tvalid = fc_tvalid && fs_tready;
    assign fc_fs_tvalid = fc_tvalid && fc_cp_tready;

    wire        fs_bs_ready_unused;
    wire [47:0] fs_to_boundary_unused;
    wire        fs_unused_ok = &{1'b0, fs_bs_ready_unused, fs_to_boundary_unused, 1'b0};

    reg [15:0] chunk_pos;
    wire       chunk_last = (chunk_pos == cfg_chunk_len - 16'd1);

    always @(posedge clk) begin
        if (rst)
            chunk_pos <= 16'd0;
        else if (fc_tvalid && fc_tready)
            chunk_pos <= chunk_last ? 16'd0 : chunk_pos + 16'd1;
    end

    frame_sync #(.ADDR_W(FS_ADDR_W)) u_fs (
        .clk                       (clk),
        .rst                       (rst),
        .cfg_fft_size              (cfg_fft_size[15:0]),
        .cfg_cp_length             (cfg_cp_length),
        .cfg_symbols_per_subframe  (16'd1),
        .cfg_subframes_per_frame   (16'd1),
        .cfg_threshold_q15         (16'sd22937),
        .cfg_lock_acquire_count    (16'd3),
        .cfg_lock_loss_count       (16'd5),
        .cfg_holdover_frames       (16'd10),
        .cfg_search_window         (16'd64),
        .cfg_fft_size_invalid      (fs_cfg_invalid),
        .chunk_overflow            (fs_chunk_overflow),
        .bs_valid                  (1'b0),
        .bs_ready                  (fs_bs_ready_unused),
        .bs_offset                 (48'd0),
        .bs_conf_q15               (16'd0),
        .s_axis_tdata              (fc_tdata),
        .s_axis_tvalid             (fc_fs_tvalid),
        .s_axis_tready             (fs_tready),
        .s_axis_tlast              (chunk_last),
        .m_axis_tdata              (fs_ev_tdata),
        .m_axis_tvalid             (fs_ev_tvalid),
        .m_axis_tready             (fs_ev_tready),
        .m_axis_tlast              (fs_ev_tlast),
        .mon_busy                  (fs_mon_busy),
        .mon_state                 (fs_mon_state),
        .mon_frame_number          (fs_mon_frame_number),
        .mon_expected_boundary     (fs_mon_expected_boundary),
        .mon_confidence            (fs_mon_confidence),
        .mon_samples_to_boundary   (fs_to_boundary_unused),
        .mon_sample_counter        (fs_mon_sample_counter)
    );

    reg [3:0] symbol_index;  // mod 16: the pilot extractor uses symbol mod Dy

    pilot_extractor #(.CP_ROM_HEX(CP_ROM_HEX)) u_pe (
        .clk                     (clk),
        .rst                     (rst),
        .cfg_fft_size            (cfg_fft_size),
        .cfg_pilot_pattern       (cfg_pilot_pattern),
        .cfg_num_active_carriers (16'd0),
        .cfg_fft_size_invalid    (pe_fft_size_invalid),
        .cfg_pattern_invalid     (pe_pattern_invalid),
        .cfg_num_active_invalid  (pe_num_active_invalid),
        .framing_error           (pe_framing_error),
        .s_axis_tdata            (fft_tdata_w),
        .s_axis_tvalid           (fft_tvalid),
        .s_axis_tready           (fft_tready),
        .s_axis_tlast            (fft_tlast),
        .s_axis_tuser            (symbol_index),
        .m_axis_tdata            (m_axis_tdata),
        .m_axis_tvalid           (m_axis_tvalid),
        .m_axis_tready           (m_axis_tready),
        .m_axis_tlast            (m_axis_tlast)
    );

    assign fft_tdata = fft_tdata_w;
    assign fft_fire  = fft_tvalid && fft_tready;

    always @(posedge clk) begin
        if (rst) begin
            fc_out_beats    <= 32'd0;
            cp_out_beats    <= 32'd0;
            cp_out_symbols  <= 32'd0;
            fft_out_symbols <= 32'd0;
            symbol_index    <= 4'd0;
        end else begin
            if (fc_tvalid && fc_tready)
                fc_out_beats <= fc_out_beats + 32'd1;
            if (cp_tvalid && cp_tready) begin
                cp_out_beats   <= cp_out_beats + 32'd1;
                cp_out_symbols <= cp_out_symbols + {31'd0, cp_tlast};
            end
            if (fft_fire && fft_tlast) begin
                fft_out_symbols <= fft_out_symbols + 32'd1;
                symbol_index    <= symbol_index + 4'd1;
            end
        end
    end

endmodule
