// SPDX-License-Identifier: CERN-OHL-W-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// channel_estimator.v — LS channel estimate, linear interpolation and
// temporal smoothing
//
// Bit-exact port of lib/channel/channel_estimator.cc ChannelEstimator::
// estimate() (ATSC3_FIXED_POINT=ON) with InterpolationMethod LINEAR (the
// C++ falls back to linear for CUBIC and WIENER as well):
//
//   pilot record -> LS multiply (H = Y / X = Y x reference, the record
//                   carrying 1/X; see pilot_extractor.v) -> linear_interp
//                -> h_average (when enabled)
//
// Not ported: estimated_snr_db and mean_phase_error (log10 / atan2 on
// float, not bit-exact portable; same exclusion as the other metrics),
// and the Wiener interpolator, whose fixed-point path takes float filter
// weights and normalises with a float divide (see hdl/docs/
// placeholder_status.md).
//
// Interface contract:
//   - Input: one PilotSymbol per beat (status_words.vh PILOT_SYMBOL_*),
//     carrier absolute, ascending within a symbol, inside the active
//     window, TLAST on the symbol's last pilot: exactly pilot_extractor's
//     output (the type field is ignored). A pilot below the first active
//     carrier is dropped like an out-of-order one (order_error).
//   - Output: one ci16 beat per active carrier, positional, TLAST on the
//     last; see linear_interp.v for the supported span.
//   - Throughput: the LS stage takes a pilot per cycle; linear_interp's
//     division per gap dominates.
//   - Reset (synchronous, active-high) clears all state and flags and
//     validates and latches the configuration (not read live):
//       cfg_fft_size not 8K/16K/32K -> 8K, cfg_fft_size_invalid
//       cfg_num_active_carriers 0   -> NoCmax for the size (6913, 13825,
//                                      27649), as the C++
//       cfg_num_active_carriers not NoCmax - Cred_coeff * Cunit with
//         Cred_coeff 0..4 (Cunit 96 / 192 / 384) -> NoCmax,
//                                      cfg_num_active_invalid (the C++
//                                      accepts any count; the RTL follows
//                                      pilot_extractor's rule so the two
//                                      blocks always agree)
//       cfg_avg_alpha_q15 > 32768 -> 32768, cfg_alpha_invalid
//     first_active = fft_size/2 - (num_active - 1)/2 (hdl/docs/
//     wire_level_layouts.md). cfg_avg_enable is read live between
//     symbols.
//
// AXI4-S: TDATA=PilotSymbol (96 bits) TVALID TREADY TLAST(symbol) in;
//         TDATA=ci16 TVALID TREADY TLAST(symbol) out.

`include "axi4s_types.vh"
`include "status_words.vh"

module channel_estimator (
    input  wire        clk,
    input  wire        rst,  // synchronous, active-high

    input  wire [17:0] cfg_fft_size,             // literal: 8192/16384/32768
    input  wire [15:0] cfg_num_active_carriers,  // 0 = default for the size
    input  wire        cfg_avg_enable,
    input  wire [15:0] cfg_avg_alpha_q15,
    output reg         cfg_fft_size_invalid,
    output reg         cfg_num_active_invalid,
    output reg         cfg_alpha_invalid,
    output wire        span_error,               // sticky
    output wire        order_error,              // sticky

    `AXI4S_SLAVE(s_axis, `PILOT_SYMBOL_WIDTH),
    `AXI4S_MASTER(m_axis, `ATSC3_SAMPLE_WIDTH)
);

    //--------------------------------------------------------------------
    // Configuration, validated now and latched during reset
    //--------------------------------------------------------------------

    wire is_8k  = (cfg_fft_size == `ATSC3_FFT_8K);
    wire is_16k = (cfg_fft_size == `ATSC3_FFT_16K);
    wire is_32k = (cfg_fft_size == `ATSC3_FFT_32K);

    reg [16:0] fft_now;
    reg [15:0] noc_max_now;
    reg [8:0]  cunit_now;
    always @(*) begin
        if (is_32k) begin
            fft_now     = 17'd32768;
            noc_max_now = 16'd27649;
            cunit_now   = 9'd384;
        end else if (is_16k) begin
            fft_now     = 17'd16384;
            noc_max_now = 16'd13825;
            cunit_now   = 9'd192;
        end else begin
            fft_now     = 17'd8192;
            noc_max_now = 16'd6913;
            cunit_now   = 9'd96;
        end
    end

    wire [15:0] diff_now = noc_max_now - cfg_num_active_carriers;
    wire [15:0] cunit16  = {7'd0, cunit_now};
    wire        have_num = (cfg_num_active_carriers != 16'd0);
    wire        le_max   = (cfg_num_active_carriers <= noc_max_now);
    reg         cred_ok;
    always @(*) begin
        cred_ok = 1'b1;
        if (diff_now == 16'd0) begin
        end else if (diff_now == cunit16) begin
        end else if (diff_now == {cunit16[14:0], 1'b0}) begin
        end else if (diff_now == cunit16 + {cunit16[14:0], 1'b0}) begin
        end else if (diff_now == {cunit16[13:0], 2'b00}) begin
        end else
            cred_ok = 1'b0;
    end
    wire        num_active_invalid_now = have_num && !(le_max && cred_ok);
    wire [15:0] num_active_now = (have_num && !num_active_invalid_now) ?
                                 cfg_num_active_carriers : noc_max_now;
    // A/322 Section 8.3: carrier 0 is (NoC - 1)/2 bins below DC (bin
    // fft/2 of the DC-centred FFT output).
    wire [16:0] first_now = (fft_now >> 1) - (({1'b0, num_active_now} - 17'd1) >> 1);

    reg [15:0] num_active;
    reg [15:0] first_active;
    reg [15:0] alpha_q15;

    always @(posedge clk) begin
        if (rst) begin
            cfg_fft_size_invalid   <= !(is_8k || is_16k || is_32k);
            cfg_num_active_invalid <= num_active_invalid_now;
            cfg_alpha_invalid      <= (cfg_avg_alpha_q15 > 16'd32768);
            num_active             <= num_active_now;
            first_active           <= first_now[15:0];
            alpha_q15              <= (cfg_avg_alpha_q15 > 16'd32768) ? 16'd32768 :
                                      cfg_avg_alpha_q15;
        end
    end

    //--------------------------------------------------------------------
    // Pilot -> LS divider
    //--------------------------------------------------------------------

    wire [15:0] abs_idx = s_axis_tdata[`PILOT_SYMBOL_SUBCARRIER_INDEX_HI:`PILOT_SYMBOL_SUBCARRIER_INDEX_LO];
    // A pilot below the window wraps to >= 65536 - 16384 (first_active is at
    // most 16384), above any num_active, so linear_interp drops it.
    wire [15:0] rel_idx = abs_idx - first_active;

    // H = Y x (1/X): the reference is real (+-1/A), so two products,
    // rounded half up and saturated to +-32767 like the C++
    // PilotSymbol::estimate_channel().
    wire signed [15:0] y_re = s_axis_tdata[`PILOT_SYMBOL_RECEIVED_RE_HI:`PILOT_SYMBOL_RECEIVED_RE_LO];
    wire signed [15:0] y_im = s_axis_tdata[`PILOT_SYMBOL_RECEIVED_IM_HI:`PILOT_SYMBOL_RECEIVED_IM_LO];
    wire signed [15:0] inv_x = s_axis_tdata[`PILOT_SYMBOL_REFERENCE_RE_HI:`PILOT_SYMBOL_REFERENCE_RE_LO];
    wire signed [31:0] p_re = y_re * inv_x;
    wire signed [31:0] p_im = y_im * inv_x;
    wire signed [31:0] r_re = (p_re + 32'sd16384) >>> 15;
    wire signed [31:0] r_im = (p_im + 32'sd16384) >>> 15;
    wire [15:0] h_re = (r_re > 32'sd32767) ? 16'sd32767 :
                       (r_re < -32'sd32767) ? -16'sd32767 : r_re[15:0];
    wire [15:0] h_im = (r_im > 32'sd32767) ? 16'sd32767 :
                       (r_im < -32'sd32767) ? -16'sd32767 : r_im[15:0];

    // One registered LS stage; a pilot is taken when the stage is empty or
    // its result is being taken, so pos_r and ls_tdata always belong to
    // the same pilot.
    reg  [31:0] ls_tdata;
    reg         ls_tvalid, ls_tlast;
    reg  [15:0] pos_r;
    wire        interp_s_ready;
    wire        ls_s_ready = !ls_tvalid || interp_s_ready;

    always @(*) s_axis_tready = ls_s_ready;

    always @(posedge clk) begin
        if (rst) begin
            ls_tvalid <= 1'b0;
            ls_tlast  <= 1'b0;
            ls_tdata  <= 32'd0;
            pos_r     <= 16'd0;
        end else if (ls_s_ready) begin
            ls_tvalid <= s_axis_tvalid;
            if (s_axis_tvalid) begin
                ls_tdata <= {h_re, h_im};
                ls_tlast <= s_axis_tlast;
                pos_r    <= rel_idx;
            end
        end
    end

    //--------------------------------------------------------------------
    // Interpolation and smoothing
    //--------------------------------------------------------------------

    wire [31:0] interp_tdata;
    wire        interp_tvalid, interp_tlast, avg_s_ready;

    linear_interp interp (
        .clk(clk), .rst(rst), .num_active(num_active),
        .span_error(span_error), .order_error(order_error),
        .s_axis_tdata({pos_r, ls_tdata}), .s_axis_tvalid(ls_tvalid),
        .s_axis_tready(interp_s_ready), .s_axis_tlast(ls_tlast),
        .m_axis_tdata(interp_tdata), .m_axis_tvalid(interp_tvalid),
        .m_axis_tready(avg_s_ready), .m_axis_tlast(interp_tlast)
    );

    wire [31:0] avg_tdata;
    wire        avg_tvalid, avg_tlast;

    h_average avg (
        .clk(clk), .rst(rst), .cfg_enable(cfg_avg_enable), .cfg_alpha(alpha_q15),
        .s_axis_tdata(interp_tdata), .s_axis_tvalid(interp_tvalid),
        .s_axis_tready(avg_s_ready), .s_axis_tlast(interp_tlast),
        .m_axis_tdata(avg_tdata), .m_axis_tvalid(avg_tvalid),
        .m_axis_tready(m_axis_tready), .m_axis_tlast(avg_tlast)
    );

    always @(*) begin
        m_axis_tdata  = avg_tdata;
        m_axis_tvalid = avg_tvalid;
        m_axis_tlast  = avg_tlast;
    end

    // Unused by design: type/reserved fields and the (zero) imaginary
    // reference of the pilot record, top bit of first_now
    wire unused_ok = &{1'b0, s_axis_tdata[95:80], first_now[16],
                        s_axis_tdata[`PILOT_SYMBOL_REFERENCE_IM_HI:`PILOT_SYMBOL_REFERENCE_IM_LO], 1'b0};

endmodule
