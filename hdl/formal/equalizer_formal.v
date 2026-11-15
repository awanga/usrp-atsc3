// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// equalizer_formal.v — Formal harness for hdl/rtl/channel/equalizer.v
//
// udiv_seq is replaced by complex_divider_stubs.v (arbitrary quotient,
// fixed latency; the real divider has its own proof). The stub asserts that
// no division is ever started with a zero divisor: that is the plan's
// "fallback divisor path is never taken" target (the C++ substituted 1
// when mag_sq >> 15 was zero; the 2^15 floor makes it unreachable).
//
// Against a ghost copy of the operands captured on the accepting edge:
//   - guard: ZF output is exactly (0, 0) when |H|^2 < min_mag_sq or < 2^15;
//     MMSE when |H|^2 + noise < 2^15 (the ghost forms |H|^2 with full-width
//     products, and the 33-bit MMSE sum);
//   - numerator width: the products and sums the RTL stores equal the
//     ghost's (no truncation), checked through the sign relation below;
//   - sign: a positive rail needs a non-negative numerator, a negative rail
//     a negative one (the stub's quotient is arbitrary, so a zero numerator
//     may still produce a nonzero rail);
//   - clamp: no rail is -32768;
//   - protocol: y and h are accepted together or not at all, one output per
//     accepted pair, TLAST follows h, framing_error exactly when a pair's
//     TLASTs differ, output held until accepted, response within 20 cycles,
//     nothing after reset.
// Quotient values, truncation toward zero and the clamp threshold are
// bit-exact in simulation against the C++ golden model (test_equalizer.py)
// with mutants, not proved here. The Q30 configuration is free but stable.
//
// The 16x16 products are cut in the .sby (cutpoint t:$mul): every property is a
// relation between values built from the same product cells (identical widths
// so the ghost's and the RTL's merge), so it holds for any product value;
// the products themselves are bit-exact in simulation against the C++ model.
//
// Bounded: BMC depth 40 (one carrier takes about 10 cycles: four carriers),
// cover depth 40. Inputs, stalls and gaps on all three interfaces are free.
//
// Run: hdl/formal/run_formal.sh equalizer

module equalizer_formal (
    input wire clk,
    input wire rst
);

    reg        cfg_mode;
    reg [30:0] cfg_min, cfg_noise;
    reg [31:0] y_tdata, h_tdata;
    reg        y_tvalid, h_tvalid, y_tlast, h_tlast, m_tready;
    wire       y_tready, h_tready, m_tvalid, m_tlast, cfg_invalid, framing_error;
    wire [31:0] m_tdata;
    wire [15:0] zeroed;

    equalizer dut (
        .clk(clk), .rst(rst),
        .cfg_mode(cfg_mode), .cfg_min_mag_sq_q30({1'b0, cfg_min}),
        .cfg_noise_q30({1'b0, cfg_noise}),
        .cfg_invalid(cfg_invalid), .framing_error(framing_error), .zeroed_carriers(zeroed),
        .y_axis_tdata(y_tdata), .y_axis_tvalid(y_tvalid), .y_axis_tready(y_tready),
        .y_axis_tlast(y_tlast),
        .h_axis_tdata(h_tdata), .h_axis_tvalid(h_tvalid), .h_axis_tready(h_tready),
        .h_axis_tlast(h_tlast),
        .m_axis_tdata(m_tdata), .m_axis_tvalid(m_tvalid), .m_axis_tready(m_tready),
        .m_axis_tlast(m_tlast)
    );

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;
    always @(*) if (!past_valid) assume (rst);
    always @(posedge clk)
        if (past_valid) assume ($stable(cfg_mode) && $stable(cfg_min) && $stable(cfg_noise));

    wire signed [15:0] y_re = y_tdata[31:16];
    wire signed [15:0] y_im = y_tdata[15:0];
    wire signed [15:0] h_re = h_tdata[31:16];
    wire signed [15:0] h_im = h_tdata[15:0];
    // Product widths match the RTL's so identical $mul cells merge and the
    // solver is not asked to prove two multiplier netlists equivalent.
    wire signed [31:0] p_yre_hre = y_re * h_re;
    wire signed [31:0] p_yim_him = y_im * h_im;
    wire signed [31:0] p_yim_hre = y_im * h_re;
    wire signed [31:0] p_yre_him = y_re * h_im;
    wire signed [33:0] num_re = {{2{p_yre_hre[31]}}, p_yre_hre} + {{2{p_yim_him[31]}}, p_yim_him};
    wire signed [33:0] num_im = {{2{p_yim_hre[31]}}, p_yim_hre} - {{2{p_yre_him[31]}}, p_yre_him};
    wire        [31:0] mag_sq = h_re * h_re + h_im * h_im;
    wire        [32:0] d_mmse = {1'b0, mag_sq} + {2'b00, cfg_noise};
    wire               zero_now = cfg_mode ? (d_mmse < 33'd32768)
                                           : ({1'b0, mag_sq} < {2'b00, cfg_min} ||
                                              mag_sq < 32'd32768);

    wire accepted = y_tvalid && y_tready;
    reg               g_active, g_zero, g_last, g_mismatch;
    reg signed [33:0] g_num_re, g_num_im;
    reg [5:0]         g_age;
    always @(posedge clk) begin
        if (rst) begin
            g_active <= 1'b0;
            g_age    <= 6'd0;
        end else begin
            if (accepted) begin
                g_active   <= 1'b1;
                g_zero     <= zero_now;
                g_last     <= h_tlast;
                g_mismatch <= (y_tlast != h_tlast);
                g_num_re   <= num_re;
                g_num_im   <= num_im;
                g_age      <= 6'd0;
            end else if (g_active && !(m_tvalid && m_tready)) begin
                g_age <= (g_age == 6'd63) ? g_age : g_age + 6'd1;
            end
            if (m_tvalid && m_tready) g_active <= 1'b0;
        end
    end

    wire signed [15:0] r_re = m_tdata[31:16];
    wire signed [15:0] r_im = m_tdata[15:0];

    reg        prev_stall, prev_last, prev_rst;
    reg [31:0] prev_data;
    initial begin
        prev_stall = 1'b0;
        prev_rst   = 1'b0;
    end
    always @(posedge clk) begin
        prev_stall <= !rst && m_tvalid && !m_tready;
        prev_data  <= m_tdata;
        prev_last  <= m_tlast;
        prev_rst   <= rst;
    end

    reg seen_mismatch;
    initial seen_mismatch = 1'b0;
    always @(posedge clk) begin
        if (rst) seen_mismatch <= 1'b0;
        else if (accepted && y_tlast != h_tlast) seen_mismatch <= 1'b1;
    end

    always @(*) begin
        if (past_valid && !rst) begin
            assert (y_tready == h_tready || !(y_tvalid && h_tvalid));
            if (y_tready) assert (h_tvalid);
            if (h_tready) assert (y_tvalid);
            if (m_tvalid) begin
                assert (g_active);
                assert (m_tlast == g_last);
                assert (r_re != -16'sd32768 && r_im != -16'sd32768);
                if (g_zero) assert (m_tdata == 32'd0);
                if (r_re > 0) assert (g_num_re >= 0);
                if (r_re < 0) assert (g_num_re < 0);
                if (r_im > 0) assert (g_num_im >= 0);
                if (r_im < 0) assert (g_num_im < 0);
            end
            if (g_active) assert (!y_tready && !h_tready);
            if (g_active && !m_tvalid) assert (g_age < 6'd20);
            assert (framing_error == seen_mismatch);
            if (prev_stall) assert (m_tvalid && m_tdata == prev_data && m_tlast == prev_last);
        end
        if (past_valid && prev_rst) assert (!m_tvalid);
    end

    always @(posedge clk) begin
        if (past_valid && !rst) begin
            cover (m_tvalid && !g_zero && r_re > 0 && r_im < 0);
            cover (m_tvalid && g_zero && cfg_mode);
            cover (m_tvalid && g_zero && !cfg_mode);
            cover (m_tvalid && g_mismatch);
        end
    end

endmodule
