// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// complex_divider_formal.v — Formal harness for hdl/rtl/channel/complex_divider.v
//
// udiv_seq is replaced by complex_divider_stubs.v (arbitrary quotient,
// fixed latency; the real divider has its own proof), so what is proved
// here is everything around it, against a ghost copy of the operands
// captured on the accepting edge:
//   - guard: |b|^2 < 2^15 gives exactly (0, 0) (and a zero divisor too);
//   - sign: a positive result rail needs a non-negative numerator (the stub
//     may return a nonzero quotient for a zero numerator), a negative
//     one a negative numerator, with the numerators formed in the harness
//     with full-width products;
//   - clamp: no rail is ever -32768 (symmetric +/-32767 saturation);
//   - protocol: s_axis_tready only while idle, exactly one result per
//     accepted operand pair, TLAST carried from that pair, TVALID and the
//     data held until accepted, result within 12 cycles, none after reset.
// The quotient magnitude itself (value, truncation toward zero, the 32767
// clamp threshold) is bit-exact in simulation against the C++ golden model
// (test_complex_divider.py) with mutants, not proved here.
//
// Bounded: BMC depth 40 (one operation is about 15 cycles; the depth
// covers two back-to-back operations), cover depth 40. Inputs are free.
//
// Run: hdl/formal/run_formal.sh complex_divider

module complex_divider_formal (
    input wire clk,
    input wire rst
);

    reg [63:0] s_tdata;
    reg        s_tvalid, s_tlast, m_tready;
    wire       s_tready, m_tvalid, m_tlast;
    wire [31:0] m_tdata;

    complex_divider dut (
        .clk(clk), .rst(rst),
        .s_axis_tdata(s_tdata), .s_axis_tvalid(s_tvalid),
        .s_axis_tready(s_tready), .s_axis_tlast(s_tlast),
        .m_axis_tdata(m_tdata), .m_axis_tvalid(m_tvalid),
        .m_axis_tready(m_tready), .m_axis_tlast(m_tlast)
    );

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;
    always @(*) if (!past_valid) assume (rst);

    // Ghost: the accepted operands
    wire signed [15:0] a_re = s_tdata[63:48];
    wire signed [15:0] a_im = s_tdata[47:32];
    wire signed [15:0] b_re = s_tdata[31:16];
    wire signed [15:0] b_im = s_tdata[15:0];
    wire signed [33:0] num_re = a_re * b_re + a_im * b_im;
    wire signed [33:0] num_im = a_im * b_re - a_re * b_im;
    wire        [31:0] mag_sq = b_re * b_re + b_im * b_im;

    reg               g_active;
    reg signed [33:0] g_num_re, g_num_im;
    reg               g_small, g_last;
    reg [4:0]         g_age;
    wire              accepted = s_tvalid && s_tready;

    always @(posedge clk) begin
        if (rst) begin
            g_active <= 1'b0;
            g_age    <= 5'd0;
        end else begin
            if (accepted) begin
                g_active <= 1'b1;
                g_num_re <= num_re;
                g_num_im <= num_im;
                g_small  <= (mag_sq < 32'd32768);
                g_last   <= s_tlast;
                g_age    <= 5'd0;
            end else if (g_active && !(m_tvalid && m_tready)) begin
                g_age <= (g_age == 5'd31) ? g_age : g_age + 5'd1;
            end
            if (m_tvalid && m_tready) g_active <= 1'b0;
        end
    end

    wire signed [15:0] r_re = m_tdata[31:16];
    wire signed [15:0] r_im = m_tdata[15:0];

    reg        prev_stall;
    reg [31:0] prev_data;
    reg        prev_last;
    reg        prev_rst;
    initial begin
        prev_stall = 1'b0;
        prev_rst   = 1'b0;
    end
    always @(posedge clk) begin
        prev_stall <= m_tvalid && !m_tready && !rst;
        prev_data  <= m_tdata;
        prev_last  <= m_tlast;
        prev_rst   <= rst;
    end

    always @(*) begin
        if (past_valid && !rst) begin
            if (m_tvalid) begin
                assert (g_active);
                assert (m_tlast == g_last);
                assert (r_re != -16'sd32768 && r_im != -16'sd32768);
                if (g_small) assert (r_re == 16'sd0 && r_im == 16'sd0);
                if (r_re > 0) assert (g_num_re >= 0);
                if (r_re < 0) assert (g_num_re < 0);
                if (r_im > 0) assert (g_num_im >= 0);
                if (r_im < 0) assert (g_num_im < 0);
            end
            if (g_active) assert (!s_tready);
            if (g_active && !m_tvalid) assert (g_age < 5'd16);
            if (prev_stall) assert (m_tvalid && m_tdata == prev_data && m_tlast == prev_last);
        end
        if (past_valid && prev_rst) assert (!m_tvalid);
    end

    always @(posedge clk) begin
        if (past_valid && !rst) begin
            cover (m_tvalid && !g_small && r_re > 0 && r_im < 0);
            cover (m_tvalid && g_small);
            cover (m_tvalid && m_tready && s_tvalid && !s_tready && g_active);
            cover (accepted && g_active == 1'b0 && m_tvalid == 1'b0 && s_tlast);
        end
    end

endmodule
