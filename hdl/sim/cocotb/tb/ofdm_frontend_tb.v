// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// ofdm_frontend_tb.v — integration top: freq_correction -> cp_removal ->
// fft_engine -> pilot_extractor -> channel_estimator -> equalizer ->
// constellation_demapper
//
// Testbench-only wiring for test_ofdm_frontend_tb.py: the blocks
// connected as in the receiver, one clock domain. The FFT output link is
// also brought out as a tap (fft_*) so the bench can check every FFT bin,
// not just the pilots. The symbol counter feeding the pilot extractor's
// TUSER stands in for frame sync's symbol index. The beat and symbol
// counters are testbench instrumentation for the status checks.

module ofdm_frontend_tb #(
    parameter TWIDDLE_HEX = "fft_twiddles.hex",
    parameter CP_ROM_HEX  = "pilot_cp_positions.hex",
    parameter NUC2D_HEX   = "nuc2d.hex",
    parameter NUC1D_HEX   = "nuc1d.hex"
) (
    input  wire        clk,
    input  wire        rst,
    input  wire [31:0] cfg_phase_inc,
    input  wire [17:0] cfg_fft_size,
    input  wire [3:0]  cfg_guard_interval,  // A/322 Table 9.13 code
    input  wire [3:0]  cfg_pilot_pattern,
    output wire        cp_fft_size_invalid,
    output wire        cp_gi_invalid,
    output wire        fft_size_invalid,
    output wire        pe_fft_size_invalid,
    output wire        pe_pattern_invalid,
    output wire        pe_num_active_invalid,
    output wire        pe_boost_invalid,
    output wire        pe_framing_error,

    input  wire [31:0] s_axis_tdata,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,

    output wire        ce_span_error,
    output wire        ce_order_error,
    output wire [31:0] ce_tdata,          // channel_estimator output
    output wire        ce_tvalid,
    output wire        ce_fire,           // estimate beat accepted by the equalizer
    output wire        ce_tlast,
    output wire [31:0] eq_tdata,          // equalizer output
    output wire        eq_tvalid,
    input  wire        eq_tready,
    output wire        eq_tlast,
    output wire        eq_framing_error,
    output wire        eq_cfg_invalid,
    output wire [7:0]  dm_tdata,          // constellation_demapper LLR output
    output wire        dm_tvalid,
    input  wire        dm_tready,
    output wire        dm_tlast,
    output wire        dm_cfg_invalid,

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
        .cfg_guard_interval         (cfg_guard_interval),
        .cfg_fft_size_invalid       (cp_fft_size_invalid),
        .cfg_guard_interval_invalid (cp_gi_invalid),
        .s_axis_tdata              (fc_tdata),
        .s_axis_tvalid             (fc_tvalid),
        .s_axis_tready             (fc_tready),
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

    wire [95:0] pe_tdata;
    wire        pe_tvalid, pe_tready, pe_tlast;
    wire        ce_fft_size_invalid_unused, ce_num_active_invalid_unused, ce_alpha_invalid_unused;

    reg [3:0] symbol_index;  // mod 16: the pilot extractor uses symbol mod Dy

    pilot_extractor #(.CP_ROM_HEX(CP_ROM_HEX)) u_pe (
        .clk                     (clk),
        .rst                     (rst),
        .cfg_fft_size            (cfg_fft_size),
        .cfg_pilot_pattern       (cfg_pilot_pattern),
        .cfg_num_active_carriers (16'd0),
        .cfg_sp_boost            (3'd0),
        .cfg_fft_size_invalid    (pe_fft_size_invalid),
        .cfg_pattern_invalid     (pe_pattern_invalid),
        .cfg_num_active_invalid  (pe_num_active_invalid),
        .cfg_boost_invalid       (pe_boost_invalid),
        .framing_error           (pe_framing_error),
        .s_axis_tdata            (fft_tdata_w),
        .s_axis_tvalid           (fft_tvalid),
        .s_axis_tready           (fft_tready),
        .s_axis_tlast            (fft_tlast),
        .s_axis_tuser            (symbol_index),
        .m_axis_tdata            (pe_tdata),
        .m_axis_tvalid           (pe_tvalid),
        .m_axis_tready           (pe_tready),
        .m_axis_tlast            (pe_tlast)
    );

    // The pilot records fork to the bench's pilot sink and the channel
    // estimator; a record moves only when both accept it.
    wire ce_s_ready;
    assign pe_tready     = m_axis_tready && ce_s_ready;
    assign m_axis_tdata  = pe_tdata;
    assign m_axis_tlast  = pe_tlast;
    assign m_axis_tvalid = pe_tvalid && ce_s_ready;

    channel_estimator u_ce (
        .clk                     (clk),
        .rst                     (rst),
        .cfg_fft_size            (cfg_fft_size),
        .cfg_num_active_carriers (16'd0),
        .cfg_avg_enable          (1'b0),
        .cfg_avg_alpha_q15       (16'd0),
        .cfg_fft_size_invalid    (ce_fft_size_invalid_unused),
        .cfg_num_active_invalid  (ce_num_active_invalid_unused),
        .cfg_alpha_invalid       (ce_alpha_invalid_unused),
        .span_error              (ce_span_error),
        .order_error             (ce_order_error),
        .s_axis_tdata            (pe_tdata),
        .s_axis_tvalid           (pe_tvalid && m_axis_tready),
        .s_axis_tready           (ce_s_ready),
        .s_axis_tlast            (pe_tlast),
        .m_axis_tdata            (ce_tdata),
        .m_axis_tvalid           (ce_tvalid),
        .m_axis_tready           (eq_h_tready),
        .m_axis_tlast            (ce_tlast)
    );

    // Equalizer: H from the estimator, Y from a bench-only buffer of the
    // active FFT bins (two banks by symbol parity). A production symbol
    // buffer and window selector are integration work; the bench only needs
    // the received bins to arrive in step with the estimates.
    wire [15:0] noc = (cfg_fft_size == 18'd32768) ? 16'd27649 :
                      (cfg_fft_size == 18'd16384) ? 16'd13825 : 16'd6913;
    // A/322 Section 8.3: carrier 0 is (NoC - 1)/2 bins below DC (bin fft/2
    // of fft_engine's DC-centred output).
    wire [16:0] first_act = cfg_fft_size[17:1] - ({1'b0, noc} - 17'd1) / 17'd2;
    reg [31:0] ybuf [0:65535];
    reg [16:0] bin_idx;
    reg        wr_bank, rd_bank;
    reg [15:0] rd_k;
    wire [16:0] rel_bin = bin_idx - first_act;
    wire        in_window = (bin_idx >= first_act) && (rel_bin < {1'b0, noc});
    wire        eq_h_tready;
    wire [31:0] y_now = ybuf[{rd_bank, rd_k[14:0]}];
    assign ce_fire = ce_tvalid && eq_h_tready;

    always @(posedge clk) begin
        if (rst) begin
            bin_idx <= 17'd0;
            wr_bank <= 1'b0;
            rd_bank <= 1'b0;
            rd_k    <= 16'd0;
        end else begin
            if (fft_fire) begin
                if (in_window)
                    ybuf[{wr_bank, rel_bin[14:0]}] <= fft_tdata_w;
                bin_idx <= fft_tlast ? 17'd0 : bin_idx + 17'd1;
                if (fft_tlast)
                    wr_bank <= !wr_bank;
            end
            if (ce_fire) begin
                rd_k <= ce_tlast ? 16'd0 : rd_k + 16'd1;
                if (ce_tlast)
                    rd_bank <= !rd_bank;
            end
        end
    end

    wire [31:0] eq_m_tdata;
    wire        eq_m_tvalid, eq_m_tlast, dm_s_ready;
    wire        eq_m_tready = eq_tready && dm_s_ready;
    wire eq_y_ready_unused;
    wire [15:0] eq_zeroed_unused;
    equalizer u_eq (
        .clk                (clk),
        .rst                (rst),
        .cfg_mode           (1'b0),
        .cfg_min_mag_sq_q30 (32'd107374),   // Equalizer defaults, from the model's K line
        .cfg_noise_q30      (32'd10737418),
        .cfg_invalid        (eq_cfg_invalid),
        .framing_error      (eq_framing_error),
        .zeroed_carriers    (eq_zeroed_unused),
        .y_axis_tdata       (y_now),
        .y_axis_tvalid      (ce_tvalid),
        .y_axis_tready      (eq_y_ready_unused),
        .y_axis_tlast       (ce_tlast),
        .h_axis_tdata       (ce_tdata),
        .h_axis_tvalid      (ce_tvalid),
        .h_axis_tready      (eq_h_tready),
        .h_axis_tlast       (ce_tlast),
        .m_axis_tdata       (eq_m_tdata),
        .m_axis_tvalid      (eq_m_tvalid),
        .m_axis_tready      (eq_m_tready),
        .m_axis_tlast       (eq_m_tlast)
    );

    // The equalized cells fork to the bench's tap and the demapper; a cell
    // moves only when both accept it.
    assign eq_tdata  = eq_m_tdata;
    assign eq_tlast  = eq_m_tlast;
    assign eq_tvalid = eq_m_tvalid && dm_s_ready;

    // 64-QAM, noise variance 0.1, clip 127: the integers are the model's K
    // line for that configuration (checked in test_ofdm_frontend_tb.py).
    constellation_demapper #(.NUC2D_HEX(NUC2D_HEX), .NUC1D_HEX(NUC1D_HEX)) u_dm (
        .clk                (clk),
        .rst                (rst),
        .cfg_modulation     (4'd2),
        .cfg_code_rate      (4'd0),
        .cfg_llr_scale_q16  (32'd745126),
        .cfg_norm_const_q15 (16'd4447),
        .cfg_llr_clip       (8'd127),
        .cfg_invalid        (dm_cfg_invalid),
        .s_axis_tdata       (eq_m_tdata),
        .s_axis_tvalid      (eq_m_tvalid && eq_tready),
        .s_axis_tready      (dm_s_ready),
        .s_axis_tlast       (eq_m_tlast),
        .m_axis_tdata       (dm_tdata),
        .m_axis_tvalid      (dm_tvalid),
        .m_axis_tready      (dm_tready),
        .m_axis_tlast       (dm_tlast)
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
