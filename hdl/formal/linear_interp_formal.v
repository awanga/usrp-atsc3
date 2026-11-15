// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// linear_interp_formal.v — Formal harness for hdl/rtl/channel/linear_interp.v
//
// Main property (data integrity vs a ghost model): for the smallest
// interesting symbol, two pilots at positions 0 and P (P free in
// 1 .. num_active-1) with free estimates in [-8, 7], num_active = 6, every
// output beat k must equal the C++ rule computed here with a plain signed
// division:
//     k < P:  h0 + trunc((h1 - h0) * k / P)        k >= P:  h1
// (the stepping arithmetic of the RTL -- one division per segment and an
// incremental remainder -- is what is being checked against it), TLAST
// exactly on beat 5, exactly num_active beats per symbol, no flag set,
// TVALID and data held until accepted.
// Shrunk to six carriers; the full 6913..27649 window and longer
// pilot lists are exercised in test_channel_estimator.py against the C++
// model. Pilot lists of three or more pilots, the leading hold, dropped
// pilots and the flags are likewise simulation-only.
//
// Tractability assumes: the sink is always ready and the source always offers
// the next pilot (stalls are simulation-covered; free per-cycle stalls made
// BMC time double with every step).
//
// Assumes (caller contract, see the RTL header): num_active == 6 held
// constant, a pilot is offered with TVALID and held until accepted, the
// symbol is exactly the two pilots above, the second one carrying TLAST.
//
// Bounded: BMC depth 40 (two pilots, two 5-cycle divisions, six beats),
// cover to the symbol's last beat.
//
// Run: hdl/formal/run_formal.sh linear_interp

module linear_interp_formal (
    input wire clk,
    input wire rst
);

    reg  [15:0]        pos1;       // P, chosen by the solver, then fixed
    reg  signed [15:0] h0_re, h0_im, h1_re, h1_im;
    reg                m_tready;

    reg  [47:0] s_tdata;
    reg         s_tvalid, s_tlast;
    wire        s_tready, m_tvalid, m_tlast, span_error, order_error;
    wire [31:0] m_tdata;

    // Divider shrunk to 4 bits (|h1 - h0| <= 15, P <= 5), 6 cycles per division
    linear_interp #(.DIV_W(4), .DIV_CNT_W(3)) dut (
        .clk(clk), .rst(rst), .num_active(16'd6),
        .span_error(span_error), .order_error(order_error),
        .s_axis_tdata(s_tdata), .s_axis_tvalid(s_tvalid),
        .s_axis_tready(s_tready), .s_axis_tlast(s_tlast),
        .m_axis_tdata(m_tdata), .m_axis_tvalid(m_tvalid),
        .m_axis_tready(m_tready), .m_axis_tlast(m_tlast)
    );

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;
    always @(*) if (!past_valid) assume (rst);

    // The two pilots are constant for the whole run
    always @(posedge clk) begin
        assume (pos1 >= 16'd1 && pos1 <= 16'd5);
        // Shrunk: estimates in [-8, 7] (the stepping is width-generic; full-range
        // values are compared bit-exactly in simulation)
        assume (h0_re >= -16'sd8 && h0_re <= 16'sd7 && h0_im >= -16'sd8 && h0_im <= 16'sd7);
        assume (h1_re >= -16'sd8 && h1_re <= 16'sd7 && h1_im >= -16'sd8 && h1_im <= 16'sd7);
        assume ($stable(pos1) && $stable(h0_re) && $stable(h0_im) &&
                $stable(h1_re) && $stable(h1_im));
    end

    // Input driver contract: pilot 0, then pilot 1 (TLAST), once; an offered
    // pilot is held until accepted.
    reg [1:0] sent;  // pilots accepted
    reg       prev_offer_stalled;
    initial begin
        sent = 2'd0;
        prev_offer_stalled = 1'b0;
    end
    always @(posedge clk) begin
        if (rst) sent <= 2'd0;
        else if (s_tvalid && s_tready) sent <= sent + 2'd1;
        prev_offer_stalled <= !rst && s_tvalid && !s_tready;
    end
    always @(*) begin
        if (!rst) begin
            assume (sent <= 2'd2);
            // Tractability: sink always ready, source always offering. Per-cycle
            // freedom here made BMC time double per step; stalls on both sides
            // are covered in test_channel_estimator.py.
            assume (m_tready);
            assume (sent == 2'd2 || s_tvalid);
            if (sent == 2'd0) begin
                assume (s_tdata == {16'd0, h0_re, h0_im} && !s_tlast);
            end else if (sent == 2'd1) begin
                assume (s_tdata == {pos1, h1_re, h1_im} && s_tlast);
            end else begin
                assume (!s_tvalid);
            end
        end
    end
    always @(posedge clk)
        if (past_valid && prev_offer_stalled) assume (s_tvalid);

    // Ghost: expected beat k
    reg [2:0] beat;  // beats accepted by the sink
    initial beat = 3'd0;
    always @(posedge clk) begin
        if (rst) beat <= 3'd0;
        else if (m_tvalid && m_tready) beat <= beat + 3'd1;
    end

    wire signed [16:0] d_re = h1_re - h0_re;
    wire signed [16:0] d_im = h1_im - h0_im;
    wire signed [23:0] p_re = d_re * $signed({1'b0, beat});
    wire signed [23:0] p_im = d_im * $signed({1'b0, beat});
    // Verilog `/` truncates toward zero; P is 1..5, so divide by constants
    reg signed [23:0] q_re, q_im;
    always @(*) begin
        case (pos1[2:0])
            3'd1:    begin q_re = p_re;     q_im = p_im;     end
            3'd2:    begin q_re = p_re / 2; q_im = p_im / 2; end
            3'd3:    begin q_re = p_re / 3; q_im = p_im / 3; end
            3'd4:    begin q_re = p_re / 4; q_im = p_im / 4; end
            default: begin q_re = p_re / 5; q_im = p_im / 5; end
        endcase
    end
    wire [15:0] e_re = ({13'd0, beat} < pos1) ? (h0_re + q_re[15:0]) : h1_re;
    wire [15:0] e_im = ({13'd0, beat} < pos1) ? (h0_im + q_im[15:0]) : h1_im;

    reg        prev_stall;
    reg [31:0] prev_data;
    reg        prev_last;
    reg        prev_rst;
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

    always @(*) begin
        if (past_valid && !rst) begin
            assert (!span_error && !order_error);
            if (m_tvalid) begin
                assert (beat <= 3'd5);
                assert (m_tdata == {e_re, e_im});
                assert (m_tlast == (beat == 3'd5));
            end
            if (prev_stall) assert (m_tvalid && m_tdata == prev_data && m_tlast == prev_last);
        end
        if (past_valid && prev_rst) assert (!m_tvalid);
    end

    always @(posedge clk) begin
        if (past_valid && !rst) begin
            cover (m_tvalid && m_tready && m_tlast && pos1 == 16'd3 && h1_re != h0_re);
            cover (m_tvalid && m_tready && m_tlast && pos1 == 16'd5 && d_im < 0);
        end
    end

endmodule
