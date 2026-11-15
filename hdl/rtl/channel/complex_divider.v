// SPDX-License-Identifier: CERN-OHL-W-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// complex_divider.v — sequential complex division a / b, Q1.15
//
// Bit-exact port of lib/channel/channel_estimator.cc safe_complex_divide()
// (ATSC3_FIXED_POINT=ON), the LS estimate H = Y / X:
//
//   mag_sq = b_re^2 + b_im^2                           (Q30, up to 2^31)
//   mag_sq < 2^15                      -> result (0, 0), no division
//   num_re = a_re*b_re + a_im*b_im,  num_im = a_im*b_re - a_re*b_im
//   result = num / (mag_sq >> 15)      truncated toward zero, Q15
//   each rail clamped to [-32767, +32767]
//
// The restoring divider (udiv_seq) works on magnitudes and the sign is
// applied afterwards, which is truncation toward zero for the C++ signed
// `/`. One udiv_seq serves both rails in turn: about 2 * 33 cycles per
// division. |num| is at most 2^31 (both operands -32768), so the
// dividend is 32 bits.
//
// Interface contract:
//   - Input: TDATA = {a_re[15:0], a_im[15:0], b_re[15:0], b_im[15:0]}
//     (numerator a = received value Y, divisor b = reference value X),
//     one division per beat. TLAST is carried through to the result.
//   - Output: TDATA = {re[15:0], im[15:0]}, valid until accepted.
//   - s_axis_tready is high only while idle, so one division is in
//     flight at a time. A zero divisor (or |b|^2 < 2^15) completes in
//     three cycles with a zero result.
//   - Reset (synchronous, active-high) abandons any division in flight.
//
// AXI4-S: TDATA=cdiv_in (64 bits) TVALID TREADY TLAST in;
//         TDATA=ci16 TVALID TREADY TLAST out.

`include "axi4s_types.vh"

module complex_divider (
    input  wire        clk,
    input  wire        rst,  // synchronous, active-high

    input  wire [(64)-1:0] s_axis_tdata,
    input  wire                    s_axis_tvalid,
    output reg                     s_axis_tready,
    input  wire                    s_axis_tlast,
    output reg  [(`ATSC3_SAMPLE_WIDTH)-1:0] m_axis_tdata,
    output reg                     m_axis_tvalid,
    input  wire                    m_axis_tready,
    output reg                     m_axis_tlast
);

    localparam [2:0] S_IDLE = 3'd0;
    localparam [2:0] S_RE   = 3'd1;  // start the real-rail division
    localparam [2:0] S_RE_W = 3'd2;
    localparam [2:0] S_IM   = 3'd3;
    localparam [2:0] S_IM_W = 3'd4;
    localparam [2:0] S_OUT  = 3'd5;

    reg [2:0] state;

    // Operand products, formed from the beat being accepted
    wire signed [15:0] a_re = s_axis_tdata[63:48];
    wire signed [15:0] a_im = s_axis_tdata[47:32];
    wire signed [15:0] b_re = s_axis_tdata[31:16];
    wire signed [15:0] b_im = s_axis_tdata[15:0];

    wire signed [31:0] p_are_bre = a_re * b_re;
    wire signed [31:0] p_aim_bim = a_im * b_im;
    wire signed [31:0] p_aim_bre = a_im * b_re;
    wire signed [31:0] p_are_bim = a_re * b_im;
    wire signed [33:0] num_re_w = {{2{p_are_bre[31]}}, p_are_bre} + {{2{p_aim_bim[31]}}, p_aim_bim};
    wire signed [33:0] num_im_w = {{2{p_aim_bre[31]}}, p_aim_bre} - {{2{p_are_bim[31]}}, p_are_bim};
    wire        [31:0] mag_sq_w = b_re * b_re + b_im * b_im;

    reg signed [33:0] num_re, num_im;
    reg        [16:0] den;       // mag_sq >> 15, 1 .. 65536
    reg               zero_div;  // mag_sq < 2^15
    reg               last_r;
    reg signed [15:0] res_re;

    wire [31:0] mag_re = num_re[33] ? (~num_re[31:0] + 32'd1) : num_re[31:0];
    wire [31:0] mag_im = num_im[33] ? (~num_im[31:0] + 32'd1) : num_im[31:0];

    reg         div_start;
    reg  [31:0] div_dividend;
    wire        div_busy;
    wire        div_done;
    wire [31:0] div_quotient;
    udiv_seq #(.WIDTH(32), .CNT_WIDTH(6)) divider (
        .clk(clk), .rst(rst),
        .start(div_start), .dividend(div_dividend), .divisor({15'd0, den}),
        .busy(div_busy), .done(div_done), .quotient(div_quotient)
    );

    always @(*) begin
        div_start    = ((state == S_RE) && !zero_div) || (state == S_IM);
        div_dividend = (state == S_IM) ? mag_im : mag_re;
        s_axis_tready = (state == S_IDLE);
    end

    // Rail result: clamp the magnitude to 32767, then restore the sign
    wire        neg_now = (state == S_IM_W) ? num_im[33] : num_re[33];
    wire [15:0] clamped = (div_quotient > 32'd32767) ? 16'd32767 : div_quotient[15:0];
    wire [15:0] rail    = neg_now ? (~clamped + 16'd1) : clamped;

    always @(posedge clk) begin
        if (rst) begin
            state         <= S_IDLE;
            m_axis_tdata  <= {`ATSC3_SAMPLE_WIDTH{1'b0}};
            m_axis_tvalid <= 1'b0;
            m_axis_tlast  <= 1'b0;
            num_re        <= 34'sd0;
            num_im        <= 34'sd0;
            den           <= 17'd0;
            zero_div      <= 1'b0;
            last_r        <= 1'b0;
            res_re        <= 16'sd0;
        end else begin
            case (state)
                S_IDLE: begin
                    if (s_axis_tvalid) begin
                        num_re   <= num_re_w;
                        num_im   <= num_im_w;
                        den      <= mag_sq_w[31:15];
                        zero_div <= (mag_sq_w[31:15] == 17'd0);
                        last_r   <= s_axis_tlast;
                        state    <= S_RE;
                    end
                end
                S_RE: begin
                    if (zero_div) begin
                        m_axis_tdata  <= {`ATSC3_SAMPLE_WIDTH{1'b0}};
                        m_axis_tvalid <= 1'b1;
                        m_axis_tlast  <= last_r;
                        state         <= S_OUT;
                    end else begin
                        state <= S_RE_W;
                    end
                end
                S_RE_W: begin
                    if (div_done) begin
                        res_re <= rail;
                        state  <= S_IM;
                    end
                end
                S_IM: state <= S_IM_W;
                S_IM_W: begin
                    if (div_done) begin
                        m_axis_tdata  <= {res_re, rail};
                        m_axis_tvalid <= 1'b1;
                        m_axis_tlast  <= last_r;
                        state         <= S_OUT;
                    end
                end
                default: begin  // S_OUT
                    if (m_axis_tready) begin
                        m_axis_tvalid <= 1'b0;
                        state         <= S_IDLE;
                    end
                end
            endcase
        end
    end

    // Unused by design: truncated high/low product bits, divider busy
    wire unused_ok = &{1'b0, mag_sq_w[14:0], num_re[32], num_im[32], div_busy, 1'b0};

endmodule
