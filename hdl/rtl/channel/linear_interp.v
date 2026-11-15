// SPDX-License-Identifier: CERN-OHL-W-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// linear_interp.v — pilot channel estimates to one estimate per carrier
//
// Bit-exact port of lib/channel/channel_estimator.cc interpolate_linear()
// (ATSC3_FIXED_POINT=ON) over the span it can meet downstream of
// pilot_extractor: between consecutive pilots (p_l, h_l) and (p_r, h_r),
//
//   h[k] = h_l + trunc((h_r - h_l) * (k - p_l) / (p_r - p_l)),  p_l <= k < p_r
//
// per rail, truncated toward zero, and h[k] = h_last from the last pilot
// to the end of the window. h_l and h_r are inside [-32767, 32767] (the
// divider clamps), so the sum stays between them and the C++'s clamp never
// fires; it is not repeated here.
//
// The division by (p_r - p_l) is done once per segment, not per carrier.
// With m = |h_r - h_l|, D = p_r - p_l, m*n = (m/D)*D*n + (m mod D)*n, so
//   floor(m*n/D) = n*floor(m/D) + floor((m mod D)*n / D)
// is stepped incrementally (carrier n -> n+1 adds floor(m/D) and one more
// when the running remainder reaches D). Truncation toward zero of a
// signed product is the sign of (h_r - h_l) times the floor of its
// magnitude, which is what the C++ `/` yields for a non-negative n.
//
// Interface contract:
//   - Input: one pilot per beat, TDATA = {pos[15:0], h_re[15:0], h_im[15:0]},
//     pos relative to the first active carrier, strictly ascending within
//     the symbol; TLAST on the symbol's last pilot. The symbol has
//     num_active carriers; num_active must be stable while a symbol is in
//     flight (the owner latches it at reset).
//   - Output: num_active ci16 beats per symbol, positional (beat n is
//     relative carrier n), TLAST on beat num_active - 1.
//   - Supported span: first pilot at position 0 (pilot_extractor always
//     puts an edge pilot there). A first pilot at p0 > 0 is held back to
//     carrier 0 (the C++ with one pilot does the same); with a second
//     pilot the C++ extrapolates linearly instead, which is not ported:
//     span_error (sticky) is set and the lead-in is held. Carriers after
//     the last pilot hold its estimate (as the C++).
//   - A pilot whose position is not above the previous one or is >=
//     num_active is dropped and order_error (sticky) is set. A symbol with
//     no usable pilot outputs zeros (the C++ zero-fills an empty list).
//   - Reset (synchronous, active-high) clears all state and the flags.
//
// AXI4-S: TDATA=pilot_est (48 bits) TVALID TREADY TLAST(symbol) in;
//         TDATA=ci16 TVALID TREADY TLAST(symbol) out.

`include "axi4s_types.vh"

module linear_interp #(
    parameter DIV_W     = 17,  // segment divider width: holds |h_r - h_l| (<= 65534)
    parameter DIV_CNT_W = 5    // must hold DIV_W
) (
    input  wire        clk,
    input  wire        rst,  // synchronous, active-high

    input  wire [15:0] num_active,
    output reg         span_error,   // sticky
    output reg         order_error,  // sticky

    input  wire [(48)-1:0] s_axis_tdata,
    input  wire                    s_axis_tvalid,
    output reg                     s_axis_tready,
    input  wire                    s_axis_tlast,
    output reg  [(`ATSC3_SAMPLE_WIDTH)-1:0] m_axis_tdata,
    output reg                     m_axis_tvalid,
    input  wire                    m_axis_tready,
    output reg                     m_axis_tlast
);

    localparam [3:0] S_WAIT  = 4'd0;
    localparam [3:0] S_DECIDE = 4'd1;
    localparam [3:0] S_LEAD  = 4'd2;
    localparam [3:0] S_RE    = 4'd3;
    localparam [3:0] S_RE_W  = 4'd4;
    localparam [3:0] S_IM    = 4'd5;
    localparam [3:0] S_IM_W  = 4'd6;
    localparam [3:0] S_SEG   = 4'd7;
    localparam [3:0] S_TRAIL = 4'd8;

    reg [3:0] state;

    // Pilot being processed
    reg [15:0]        in_pos;
    reg signed [15:0] in_re, in_im;
    reg               in_last;

    // Left pilot of the current segment
    reg               have_left;
    reg               any_seg;      // a segment has been emitted this symbol
    reg [15:0]        left_pos;
    reg signed [15:0] left_re, left_im;

    reg [15:0] k;  // next carrier to emit

    // Per-segment stepping state, one set per rail
    reg [15:0] seg_len;  // D
    reg        neg_re, neg_im;
    reg [16:0] stq_re, stq_im;  // floor(m / D)
    reg [15:0] str_re, str_im;  // m mod D
    reg [16:0] q_re, q_im;      // floor(m * n / D)
    reg [15:0] r_re, r_im;      // (m * n) mod D

    wire [15:0] max_pos_ok = num_active - 16'd1;
    wire        in_ok = (in_pos <= max_pos_ok) && (!have_left || (in_pos > left_pos));

    wire signed [16:0] d_re = {in_re[15], in_re} - {left_re[15], left_re};
    wire signed [16:0] d_im = {in_im[15], in_im} - {left_im[15], left_im};
    wire [16:0] m_re = d_re[16] ? (~d_re + 17'd1) : d_re;
    wire [16:0] m_im = d_im[16] ? (~d_im + 17'd1) : d_im;

    reg         div_start;
    reg  [16:0] div_dividend;  // |h_r - h_l| of the rail being divided
    wire        div_busy;
    wire        div_done;
    wire [DIV_W-1:0] div_quotient;
    wire [31:0] seg32 = {16'd0, seg_len};
    wire [31:0] dvd32 = {15'd0, div_dividend};
    udiv_seq #(.WIDTH(DIV_W), .CNT_WIDTH(DIV_CNT_W)) divider (
        .clk(clk), .rst(rst),
        .start(div_start), .dividend(dvd32[DIV_W-1:0]), .divisor(seg32[DIV_W-1:0]),
        .busy(div_busy), .done(div_done), .quotient(div_quotient)
    );

    wire [31:0] quo32 = {{(32-DIV_W){1'b0}}, div_quotient};
    wire [31:0] stq_times_len = quo32 * seg32;  // <= m, fits 17 bits
    wire [16:0] rem_now = div_dividend - stq_times_len[16:0];

    // Stepped value: left +/- q
    wire signed [17:0] val_re_w = neg_re ? ({left_re[15], left_re[15], left_re} - {1'b0, q_re})
                                         : ({left_re[15], left_re[15], left_re} + {1'b0, q_re});
    wire signed [17:0] val_im_w = neg_im ? ({left_im[15], left_im[15], left_im} - {1'b0, q_im})
                                         : ({left_im[15], left_im[15], left_im} + {1'b0, q_im});

    wire [16:0] rsum_re = {1'b0, r_re} + {1'b0, str_re};
    wire [16:0] rsum_im = {1'b0, r_im} + {1'b0, str_im};
    wire        wrap_re = (rsum_re >= {1'b0, seg_len});
    wire        wrap_im = (rsum_im >= {1'b0, seg_len});

    wire beat_ok = !m_axis_tvalid || m_axis_tready;

    always @(*) begin
        s_axis_tready = (state == S_WAIT);
        div_start     = (state == S_RE) || (state == S_IM);
        div_dividend  = (state == S_IM || state == S_IM_W) ? m_im : m_re;
    end

    always @(posedge clk) begin
        if (rst) begin
            state         <= S_WAIT;
            span_error    <= 1'b0;
            order_error   <= 1'b0;
            m_axis_tdata  <= {`ATSC3_SAMPLE_WIDTH{1'b0}};
            m_axis_tvalid <= 1'b0;
            m_axis_tlast  <= 1'b0;
            in_pos        <= 16'd0;
            in_re         <= 16'sd0;
            in_im         <= 16'sd0;
            in_last       <= 1'b0;
            have_left     <= 1'b0;
            any_seg       <= 1'b0;
            left_pos      <= 16'd0;
            left_re       <= 16'sd0;
            left_im       <= 16'sd0;
            k             <= 16'd0;
            seg_len       <= 16'd1;
            neg_re        <= 1'b0;
            neg_im        <= 1'b0;
            stq_re        <= 17'd0;
            stq_im        <= 17'd0;
            str_re        <= 16'd0;
            str_im        <= 16'd0;
            q_re          <= 17'd0;
            q_im          <= 17'd0;
            r_re          <= 16'd0;
            r_im          <= 16'd0;
        end else begin
            if (m_axis_tvalid && m_axis_tready)
                m_axis_tvalid <= 1'b0;

            case (state)
                S_WAIT: begin
                    if (s_axis_tvalid) begin
                        in_pos  <= s_axis_tdata[47:32];
                        in_re   <= s_axis_tdata[31:16];
                        in_im   <= s_axis_tdata[15:0];
                        in_last <= s_axis_tlast;
                        state   <= S_DECIDE;
                    end
                end

                S_DECIDE: begin
                    if (!in_ok) begin
                        order_error <= 1'b1;
                        if (!in_last) begin
                            state <= S_WAIT;
                        end else begin
                            if (!have_left) begin  // nothing usable: zeros
                                left_pos  <= 16'd0;
                                left_re   <= 16'sd0;
                                left_im   <= 16'sd0;
                                have_left <= 1'b1;
                            end
                            state <= S_TRAIL;
                        end
                    end else if (!have_left) begin
                        left_pos  <= in_pos;
                        left_re   <= in_re;
                        left_im   <= in_im;
                        have_left <= 1'b1;
                        any_seg   <= 1'b0;
                        state     <= (in_pos != 16'd0) ? S_LEAD :
                                     in_last ? S_TRAIL : S_WAIT;
                    end else begin
                        if (!any_seg && left_pos != 16'd0)
                            span_error <= 1'b1;
                        seg_len <= in_pos - left_pos;
                        neg_re  <= d_re[16];
                        neg_im  <= d_im[16];
                        state   <= S_RE;
                    end
                end

                S_LEAD: begin
                    if (k == left_pos) begin
                        state <= in_last ? S_TRAIL : S_WAIT;
                    end else if (beat_ok) begin
                        m_axis_tdata  <= {left_re, left_im};
                        m_axis_tvalid <= 1'b1;
                        m_axis_tlast  <= 1'b0;
                        k             <= k + 16'd1;
                    end
                end

                S_RE: state <= S_RE_W;
                S_RE_W: begin
                    if (div_done) begin
                        stq_re <= quo32[16:0];
                        str_re <= rem_now[15:0];
                        state  <= S_IM;
                    end
                end
                S_IM: state <= S_IM_W;
                S_IM_W: begin
                    if (div_done) begin
                        stq_im <= quo32[16:0];
                        str_im <= rem_now[15:0];
                        q_re   <= 17'd0;
                        q_im   <= 17'd0;
                        r_re   <= 16'd0;
                        r_im   <= 16'd0;
                        state  <= S_SEG;
                    end
                end

                S_SEG: begin
                    if (beat_ok) begin
                        m_axis_tdata  <= {val_re_w[15:0], val_im_w[15:0]};
                        m_axis_tvalid <= 1'b1;
                        m_axis_tlast  <= 1'b0;
                        k             <= k + 16'd1;
                        q_re <= q_re + stq_re + {16'd0, wrap_re};
                        q_im <= q_im + stq_im + {16'd0, wrap_im};
                        r_re <= wrap_re ? (rsum_re[15:0] - seg_len) : rsum_re[15:0];
                        r_im <= wrap_im ? (rsum_im[15:0] - seg_len) : rsum_im[15:0];
                        if (k + 16'd1 == in_pos) begin
                            left_pos <= in_pos;
                            left_re  <= in_re;
                            left_im  <= in_im;
                            any_seg  <= 1'b1;
                            state    <= in_last ? S_TRAIL : S_WAIT;
                        end
                    end
                end

                default: begin  // S_TRAIL
                    if (beat_ok) begin
                        m_axis_tdata  <= {left_re, left_im};
                        m_axis_tvalid <= 1'b1;
                        m_axis_tlast  <= (k == max_pos_ok);
                        k             <= k + 16'd1;
                        if (k == max_pos_ok) begin
                            k         <= 16'd0;
                            have_left <= 1'b0;
                            any_seg   <= 1'b0;
                            state     <= S_WAIT;
                        end
                    end
                end
            endcase
        end
    end

    // Unused by design: widened sums whose top bits are sign copies, divider busy
    wire unused_ok = &{1'b0, div_busy, stq_times_len[31:17], quo32[31:17], dvd32[31:17], rem_now[16], val_re_w[17:16],
                       val_im_w[17:16], 1'b0};

endmodule
