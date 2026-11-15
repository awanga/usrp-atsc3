// SPDX-License-Identifier: CERN-OHL-W-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// equalizer.v — single-tap frequency-domain equalizer, ZF and MMSE
//
// Bit-exact port of lib/channel/equalizer.cc Equalizer::equalize_sample()
// (ATSC3_FIXED_POINT=ON), the per-carrier core of equalize():
//
//   mag_sq   = h_re^2 + h_im^2                                (Q30, up to 2^31)
//   num_re   = y_re*h_re + y_im*h_im,  num_im = y_im*h_re - y_re*h_im
//   ZF:      zero if mag_sq < min_mag_sq or mag_sq < 2^15;  den = mag_sq >> 15
//   MMSE:    d = mag_sq + noise;  zero if d < 2^15;         den = d >> 15
//            (MMSE does not consult min_mag_sq, as in the C++)
//   out      = num / den truncated toward zero, each rail clamped to
//              [-32767, +32767]
//
// min_mag_sq and noise are the Q30 integers Equalizer::
// min_magnitude_sq_q30() / noise_q30() derive from the float
// configuration; software computes them and writes them here. The deep
// fade floor 2^15 is what keeps den >= 1, so the C++'s divide-by-zero
// fallback is unreachable (the formal harness asserts it).
//
// Not ported: equalize_with_pilots()'s residual-phase tracking and
// phase rotation (atan2/sin/cos on float; not bit-exact portable, same
// exclusion as the other trigonometric and log10 metrics), the EVM
// estimate and the symbol-level result flags other than the zeroed
// count below.
//
// Two udiv_seq instances divide the real and imaginary numerators in
// parallel: about 35 cycles per carrier.
//
// Interface contract:
//   - Inputs: y (received, positional active carriers) and h (channel
//     estimate, positional), ci16 {re, im}, consumed in lock-step: a
//     carrier is accepted only when both streams offer one, the same
//     cycle. TLAST on each stream marks the symbol's last carrier; they
//     must agree, else framing_error (sticky) is set and the y TLAST is
//     ignored (the output TLAST follows h).
//   - Output: ci16 {re, im}, one per carrier, held until accepted, TLAST
//     as above.
//   - zeroed_carriers: count of carriers in the current symbol whose
//     output is exactly (0, 0), including genuine zero quotients (the C++
//     num_zeroed_carriers); valid once the symbol's TLAST beat is
//     presented, restarts at the next symbol's first beat.
//   - Configuration (latched at reset, not read live): cfg_mode 0 = ZF,
//     1 = MMSE; cfg_min_mag_sq_q30 and cfg_noise_q30 must be below 2^31
//     (the C++ int32 conversion); a value with bit 31 set is clamped to
//     2^31 - 1 and cfg_invalid is set.
//   - Reset (synchronous, active-high) clears all state and flags.
//
// AXI4-S: y: TDATA=ci16 TVALID TREADY TLAST; h: TDATA=ci16 TVALID TREADY
//         TLAST; out: TDATA=ci16 TVALID TREADY TLAST.

module equalizer (
    input  wire        clk,
    input  wire        rst,  // synchronous, active-high

    input  wire        cfg_mode,
    input  wire [31:0] cfg_min_mag_sq_q30,
    input  wire [31:0] cfg_noise_q30,
    output reg         cfg_invalid,     // sticky until reset
    output reg         framing_error,   // sticky until reset
    output reg  [15:0] zeroed_carriers,

    input  wire [31:0] y_axis_tdata,
    input  wire        y_axis_tvalid,
    output reg         y_axis_tready,
    input  wire        y_axis_tlast,

    input  wire [31:0] h_axis_tdata,
    input  wire        h_axis_tvalid,
    output reg         h_axis_tready,
    input  wire        h_axis_tlast,

    output reg  [31:0] m_axis_tdata,
    output reg         m_axis_tvalid,
    input  wire        m_axis_tready,
    output reg         m_axis_tlast
);

    localparam [1:0] S_IDLE = 2'd0;
    localparam [1:0] S_RUN  = 2'd1;  // dividers started; wait for done
    localparam [1:0] S_OUT  = 2'd2;

    reg [1:0] state;
    reg       start;       // one-cycle divider start
    reg       mode_r;
    reg [30:0] min_mag_r;
    reg [30:0] noise_r;

    wire signed [15:0] y_re = y_axis_tdata[31:16];
    wire signed [15:0] y_im = y_axis_tdata[15:0];
    wire signed [15:0] h_re = h_axis_tdata[31:16];
    wire signed [15:0] h_im = h_axis_tdata[15:0];

    wire signed [31:0] p_yre_hre = y_re * h_re;
    wire signed [31:0] p_yim_him = y_im * h_im;
    wire signed [31:0] p_yim_hre = y_im * h_re;
    wire signed [31:0] p_yre_him = y_re * h_im;
    wire signed [33:0] num_re_w = {{2{p_yre_hre[31]}}, p_yre_hre} + {{2{p_yim_him[31]}}, p_yim_him};
    wire signed [33:0] num_im_w = {{2{p_yim_hre[31]}}, p_yim_hre} - {{2{p_yre_him[31]}}, p_yre_him};
    wire        [31:0] mag_sq_w = h_re * h_re + h_im * h_im;
    wire        [32:0] den_full_w = mode_r ? ({1'b0, mag_sq_w} + {2'b00, noise_r}) : {1'b0, mag_sq_w};
    wire               zero_w = mode_r ? (den_full_w < 33'd32768)
                                       : ({1'b0, mag_sq_w} < {2'b00, min_mag_r} ||
                                          mag_sq_w < 32'd32768);

    reg signed [33:0] num_re, num_im;
    reg        [17:0] den;       // den_full >> 15, 1 .. 131072 (when not zeroed)
    reg               zero_r;
    reg               last_r;
    reg               sym_start;  // the next output is the symbol's first

    wire [31:0] mag_re = num_re[33] ? (~num_re[31:0] + 32'd1) : num_re[31:0];
    wire [31:0] mag_im = num_im[33] ? (~num_im[31:0] + 32'd1) : num_im[31:0];

    wire        busy_re, busy_im, done_re, done_im;
    wire [31:0] quo_re, quo_im;
    udiv_seq #(.WIDTH(32), .CNT_WIDTH(6)) div_re (
        .clk(clk), .rst(rst), .start(start), .dividend(mag_re), .divisor({14'd0, den}),
        .busy(busy_re), .done(done_re), .quotient(quo_re)
    );
    udiv_seq #(.WIDTH(32), .CNT_WIDTH(6)) div_im (
        .clk(clk), .rst(rst), .start(start), .dividend(mag_im), .divisor({14'd0, den}),
        .busy(busy_im), .done(done_im), .quotient(quo_im)
    );

    wire [15:0] cl_re = (quo_re > 32'd32767) ? 16'd32767 : quo_re[15:0];
    wire [15:0] cl_im = (quo_im > 32'd32767) ? 16'd32767 : quo_im[15:0];
    wire [15:0] rail_re = num_re[33] ? (~cl_re + 16'd1) : cl_re;
    wire [15:0] rail_im = num_im[33] ? (~cl_im + 16'd1) : cl_im;

    wire pair_valid = y_axis_tvalid && h_axis_tvalid;

    always @(*) begin
        y_axis_tready = (state == S_IDLE) && h_axis_tvalid;
        h_axis_tready = (state == S_IDLE) && y_axis_tvalid;
    end

    wire [31:0] out_now = zero_r ? 32'd0 : {rail_re, rail_im};

    always @(posedge clk) begin
        if (rst) begin
            state           <= S_IDLE;
            start           <= 1'b0;
            mode_r          <= cfg_mode;
            min_mag_r       <= cfg_min_mag_sq_q30[31] ? 31'h7FFFFFFF : cfg_min_mag_sq_q30[30:0];
            noise_r         <= cfg_noise_q30[31] ? 31'h7FFFFFFF : cfg_noise_q30[30:0];
            cfg_invalid     <= cfg_min_mag_sq_q30[31] || cfg_noise_q30[31];
            framing_error   <= 1'b0;
            zeroed_carriers <= 16'd0;
            m_axis_tdata    <= 32'd0;
            m_axis_tvalid   <= 1'b0;
            m_axis_tlast    <= 1'b0;
            num_re          <= 34'sd0;
            num_im          <= 34'sd0;
            den             <= 18'd1;
            zero_r          <= 1'b0;
            last_r          <= 1'b0;
            sym_start       <= 1'b1;
        end else begin
            start <= 1'b0;
            case (state)
                S_IDLE: begin
                    if (pair_valid) begin
                        num_re <= num_re_w;
                        num_im <= num_im_w;
                        den    <= den_full_w[32:15];
                        zero_r <= zero_w;
                        last_r <= h_axis_tlast;
                        if (y_axis_tlast != h_axis_tlast)
                            framing_error <= 1'b1;
                        if (zero_w) begin
                            state <= S_OUT;
                        end else begin
                            start <= 1'b1;
                            state <= S_RUN;
                        end
                    end
                end
                S_RUN: begin
                    if (done_re && done_im)
                        state <= S_OUT;
                end
                default: begin  // S_OUT: present, then wait for acceptance
                    if (!m_axis_tvalid) begin
                        m_axis_tdata  <= out_now;
                        m_axis_tvalid <= 1'b1;
                        m_axis_tlast  <= last_r;
                        zeroed_carriers <= (sym_start ? 16'd0 : zeroed_carriers) +
                                           {15'd0, (out_now == 32'd0)};
                        sym_start     <= last_r;
                    end else if (m_axis_tready) begin
                        m_axis_tvalid <= 1'b0;
                        state         <= S_IDLE;
                    end
                end
            endcase
        end
    end

    // Unused by design: divider busy, widened product/sum sign bits
    wire unused_ok = &{1'b0, busy_re, busy_im, num_re[32], num_im[32], 1'b0};

endmodule
