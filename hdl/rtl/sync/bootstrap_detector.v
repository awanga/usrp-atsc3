// bootstrap_detector.v — ATSC 3.0 bootstrap detection + coarse CFO estimate
//
// Bit-exact port of lib/sync/bootstrap_detector.cc's ATSC3_FIXED_POINT path
// (Schmidl-Cox autocorrelation at lag L = HALF_SYMBOL = 2048). Per input
// sample, in the same order as process_sample() then check_detection():
//
//   1. P (correlation) EWMA, alpha = 1/1024:  p = p - (p >>> 10) + 2*corr,
//      corr = x[n] * conj(x[n-L]). A single-pole accumulator, not a
//      2048-tap delay line -- the golden model's actual architecture.
//      R (power) is a true L-sample sliding window, floored at 1.
//   2. norm = (P * 32768) / R per rail, C++ truncate-toward-zero division,
//      then the golden model's block-floating-point shift loop (arithmetic
//      >>1 on both rails until both fit +-32767, at most 32 times).
//   3. Shared CORDIC core (hdl/rtl/common/cordic.v), vectoring mode:
//      magnitude and angle of the normalized P.
//   4. metric = (magnitude << shift)^2 >> 15, saturated to INT32_MAX once
//      magnitude >= 2^23 (matches the golden model's overflow guard).
//   5. Moving average of metric over the averaging window (history RAM +
//      running sum), divided by the window length.
//   6. Detection FSM: arm when smoothed > threshold, track the peak, fire
//      on the falling edge below 0.8 * peak (26214 in Q1.15, which is
//      float_to_q15(0.8f)'s truncated value).
//
// Every arithmetic width mirrors the C++ int64_t/int32_t/int16_t choices
// directly rather than a tightened width, same policy as cordic.v; width
// optimization is timing-closure work.
//
// Throughput: this is a sequential datapath (two 64-cycle divides, up to
// 32 normalization shifts, the iterative CORDIC), roughly 150-190 clock
// cycles per input sample. At the 100 MHz nominal clock that is well
// below the 6.25 MS/s sample rate. Correctness first; faster dividers and
// a pipelined datapath are the timing-closure work tracked in TASKS.md.
// s_axis_tready backpressures the input while a sample is in flight.
//
// AXI4-S: TDATA=ci16 {re[15:0], im[15:0]} TVALID TREADY in (TLAST
// ignored, the golden model has no input framing). Out: one
// BootstrapDetection status word per detection (status_words.vh), TLAST
// always 1. A pending detection holds off the next input sample until
// m_axis_tready accepts it.
//
// Config: cfg_sample_rate_hz and cfg_threshold_q15 are read live, as the
// C++ reads config_ on every call. cfg_averaging_window is latched while
// rst is high, since the history RAM's length only changes on
// construction/set_config() in C++; change it by resetting the block. 0 is
// clamped to 1 (the golden model's own clamp). Values above
// 2^MAX_WIN_LOG2 are clamped to that and flagged on cfg_window_clamped,
// where the RTL knowingly diverges from the unbounded C++ vector.
//
// Monitor outputs (mon_*) pulse once per processed sample with the
// current smoothed metric and CFO, the RTL counterparts of
// BootstrapDetector::get_current_metric()/get_current_cfo_hz() (both
// scaled as integers here, the C++ returns metric / 32768.0).
//
// After reset the block spends max(2^HALF_SYMBOL_LOG2, 2^MAX_WIN_LOG2)
// cycles zeroing its RAMs (matching the C++ constructor's zero-filled
// vectors) before raising s_axis_tready.

`include "axi4s_types.vh"
`include "cordic_types.vh"
`include "status_words.vh"

module bootstrap_detector #(
    parameter HALF_SYMBOL_LOG2 = 11,  // L = 2048, kHalfSymbol; fixed by the spec
    parameter MAX_WIN_LOG2     = 10   // averaging-window RAM depth, 1024
) (
    input  wire                clk,
    input  wire                rst,  // synchronous, active-high

    input  wire [31:0]         cfg_sample_rate_hz,
    input  wire signed [15:0]  cfg_threshold_q15,
    input  wire [31:0]         cfg_averaging_window,
    output reg                 cfg_window_clamped,

    `AXI4S_SLAVE(s_axis, `ATSC3_SAMPLE_WIDTH),
    `AXI4S_MASTER(m_axis, `BOOTSTRAP_DETECTION_WIDTH),

    output reg                 mon_valid,
    output reg  [31:0]         mon_metric,
    output reg  signed [31:0]  mon_cfo_hz
);

    localparam HALF_SYMBOL = 1 << HALF_SYMBOL_LOG2;
    localparam MAX_WIN     = 1 << MAX_WIN_LOG2;
    localparam CLR_LOG2    = (HALF_SYMBOL_LOG2 > MAX_WIN_LOG2) ? HALF_SYMBOL_LOG2
                                                               : MAX_WIN_LOG2;
    localparam CLR_LAST    = (1 << CLR_LOG2) - 1;

    localparam [3:0] ST_CLEAR        = 4'd0,
                     ST_IDLE         = 4'd1,
                     ST_ACC          = 4'd2,
                     ST_DIV_START    = 4'd3,
                     ST_DIV_WAIT     = 4'd4,
                     ST_SHIFT        = 4'd5,
                     ST_CORDIC_START = 4'd6,
                     ST_CORDIC_WAIT  = 4'd7,
                     ST_METRIC       = 4'd8,
                     ST_SUM          = 4'd9,
                     ST_AVG_WAIT     = 4'd10,
                     ST_DETECT       = 4'd11,
                     ST_EMIT         = 4'd12;

    // float_to_q15(0.8f), truncated, as in check_detection()'s falling edge
    localparam signed [63:0] FALLING_Q15 = 64'sd26214;
    // magnitude at which (magnitude^2) >> 15 no longer fits int32_t
    localparam [63:0] METRIC_SAT_MAG = 64'd1 << 23;
    localparam [31:0] INT32_MAX      = 32'h7FFF_FFFF;

    reg [3:0] state;

    //--------------------------------------------------------------------------
    // RAMs: delay line x[n-L], its power |x[n-L]|^2 at the time it was
    // delayed (power_buffer_), and the metric history. Plain inferred
    // synchronous-read memories; zeroed by the post-reset clear sweep.
    //--------------------------------------------------------------------------

    reg [31:0] delay_mem [0:HALF_SYMBOL-1];
    reg [31:0] power_mem [0:HALF_SYMBOL-1];
    reg [31:0] hist_mem  [0:MAX_WIN-1];

    reg [31:0] delay_rd;
    reg [31:0] power_rd;
    reg [31:0] hist_rd;

    reg [CLR_LOG2:0]           clr_idx;
    reg [HALF_SYMBOL_LOG2-1:0] idx;   // delay_idx_ == power_idx_ in C++
    reg [MAX_WIN_LOG2-1:0]     hidx;  // metric_idx_
    reg [MAX_WIN_LOG2:0]       win_n; // effective window, 1..MAX_WIN

    //--------------------------------------------------------------------------
    // Datapath state
    //--------------------------------------------------------------------------

    reg signed [15:0] x_re, x_im;
    reg signed [63:0] p_re, p_im;   // p_sum_re_/p_sum_im_
    reg signed [63:0] r_sum;        // r_sum_
    reg [47:0]        sample_count; // status-word width (see status_words.vh)

    reg               p_re_neg, p_im_neg;
    reg signed [63:0] norm_re, norm_im;
    reg [5:0]         shift;

    reg signed [15:0] angle;
    reg [31:0]        magnitude;
    reg [31:0]        metric;
    reg [63:0]        metric_sum;  // sum of hist_mem, always >= 0
    reg [63:0]        smoothed;

    reg               in_det;
    reg [31:0]        peak_metric;
    reg [47:0]        peak_sample;
    reg signed [15:0] peak_angle;

    //--------------------------------------------------------------------------
    // Step 1: correlation/power update (ST_ACC)
    //--------------------------------------------------------------------------

    wire signed [15:0] xd_re = delay_rd[31:16];
    wire signed [15:0] xd_im = delay_rd[15:0];

    // Each 16x16 signed product fits int32 (max (-32768)^2 = 2^30); sums
    // are formed at 64 bits like the C++ int64_t locals.
    wire signed [31:0] m_rr = x_re * xd_re;
    wire signed [31:0] m_ii = x_im * xd_im;
    wire signed [31:0] m_ir = x_im * xd_re;
    wire signed [31:0] m_ri = x_re * xd_im;
    wire signed [31:0] m_dd_r = xd_re * xd_re;
    wire signed [31:0] m_dd_i = xd_im * xd_im;

    wire signed [63:0] corr_re = {{32{m_rr[31]}}, m_rr} + {{32{m_ii[31]}}, m_ii};
    wire signed [63:0] corr_im = {{32{m_ir[31]}}, m_ir} - {{32{m_ri[31]}}, m_ri};
    // up to 2^31, so formed at 64 bits; stored in power_mem as 32-bit unsigned
    wire signed [63:0] new_power = {{32{m_dd_r[31]}}, m_dd_r} + {{32{m_dd_i[31]}}, m_dd_i};
    wire signed [63:0] old_power = {32'd0, power_rd};

    wire signed [63:0] r_raw  = r_sum - old_power + new_power;
    wire signed [63:0] r_next = (r_raw < 64'sd1) ? 64'sd1 : r_raw;

    wire signed [63:0] p_re_next = p_re - (p_re >>> 10) + (corr_re <<< 1);
    wire signed [63:0] p_im_next = p_im - (p_im >>> 10) + (corr_im <<< 1);

    //--------------------------------------------------------------------------
    // Step 2: (P * 32768) / R, truncating toward zero: divide magnitudes,
    // restore the sign. |P| < 2^44 by the EWMA's bound, so |P| << 15 fits.
    //--------------------------------------------------------------------------

    wire signed [63:0] p_re_abs = p_re[63] ? -p_re : p_re;
    wire signed [63:0] p_im_abs = p_im[63] ? -p_im : p_im;

    reg         div_start;
    reg         div_avg;  // divider A: 0 = P_re/R, 1 = metric_sum/window
    reg  [63:0] div_a_dividend, div_a_divisor;
    wire        div_a_busy, div_a_done, div_b_busy, div_b_done;
    wire [63:0] div_a_quotient, div_b_quotient;

    udiv_seq #(.WIDTH(64), .CNT_WIDTH(7)) div_a (
        .clk      (clk),
        .rst      (rst),
        .start    (div_start),
        .dividend (div_a_dividend),
        .divisor  (div_a_divisor),
        .busy     (div_a_busy),
        .done     (div_a_done),
        .quotient (div_a_quotient)
    );

    udiv_seq #(.WIDTH(64), .CNT_WIDTH(7)) div_b (
        .clk      (clk),
        .rst      (rst),
        .start    (div_start && !div_avg),
        .dividend (p_im_abs <<< 15),
        .divisor  (r_sum),
        .busy     (div_b_busy),
        .done     (div_b_done),
        .quotient (div_b_quotient)
    );

    wire signed [63:0] quo_a = div_a_quotient;
    wire signed [63:0] quo_b = div_b_quotient;

    wire norm_out_of_range = (norm_re > 64'sd32767) || (norm_re < -64'sd32767) ||
                             (norm_im > 64'sd32767) || (norm_im < -64'sd32767);

    function signed [15:0] saturate_i16;
        input signed [63:0] v;
        begin
            if (v > 64'sd32767)
                saturate_i16 = 16'sd32767;
            else if (v < -64'sd32767)
                saturate_i16 = -16'sd32767;
            else
                saturate_i16 = v[15:0];
        end
    endfunction

    //--------------------------------------------------------------------------
    // Step 3: shared CORDIC core, vectoring mode
    //--------------------------------------------------------------------------

    reg                cordic_start;
    wire               cordic_busy, cordic_done;
    wire signed [15:0] cordic_angle;
    wire signed [31:0] cordic_mag;

    cordic u_cordic (
        .clk   (clk),
        .rst   (rst),
        .start (cordic_start),
        .mode  (`CORDIC_MODE_VECTOR),
        .in_a  (saturate_i16(norm_re)),
        .in_b  (saturate_i16(norm_im)),
        .busy  (cordic_busy),
        .done  (cordic_done),
        .out_a (cordic_angle),
        .out_b (cordic_mag)
    );

    //--------------------------------------------------------------------------
    // Step 4/5: metric and its moving average
    //--------------------------------------------------------------------------

    // CORDIC magnitude is never negative and at most ~46341, so << 32
    // still fits comfortably in 64 bits.
    wire [63:0] mag_shifted = {32'd0, magnitude} << shift;
    wire [45:0] mag_sq      = mag_shifted[22:0] * mag_shifted[22:0];
    wire [31:0] metric_next = (mag_shifted >= METRIC_SAT_MAG) ? INT32_MAX
                                                              : {1'b0, mag_sq[45:15]};

    wire [63:0] sum_next = metric_sum - {32'd0, hist_rd} + {32'd0, metric};

    //--------------------------------------------------------------------------
    // Step 6: detection
    //--------------------------------------------------------------------------

    wire signed [63:0] smoothed_s = smoothed;
    wire signed [63:0] threshold  = {{48{cfg_threshold_q15[15]}}, cfg_threshold_q15};

    wire        new_peak     = smoothed_s > $signed({32'd0, peak_metric});
    wire [31:0] peak_next    = new_peak ? smoothed[31:0] : peak_metric;
    wire [47:0] peak_s_next  = new_peak ? sample_count : peak_sample;
    wire signed [15:0] peak_a_next = new_peak ? angle : peak_angle;
    wire signed [63:0] falling_thr = ($signed({32'd0, peak_next}) * FALLING_Q15) >>> 15;
    wire        falling_edge = smoothed_s < falling_thr;

    // cfo_hz = (angle * Fs) >> 27; see check_detection() for why pi
    // cancels. 16 x 33 signed bits; |result| < 2^21, so the low 32 bits of
    // the shifted product are the exact int32 value.
    wire signed [32:0] fs_s          = {1'b0, cfg_sample_rate_hz};
    wire signed [48:0] cfo_prod_cur  = angle * fs_s;
    wire signed [48:0] cfo_prod_peak = peak_a_next * fs_s;
    wire signed [48:0] cfo_shr_cur   = cfo_prod_cur >>> 27;
    wire signed [48:0] cfo_shr_peak  = cfo_prod_peak >>> 27;

    //--------------------------------------------------------------------------
    // RAM ports
    //--------------------------------------------------------------------------

    wire clearing = (state == ST_CLEAR);
    wire dly_we   = (clearing && (clr_idx < HALF_SYMBOL)) || (state == ST_ACC);
    wire hist_we  = (clearing && (clr_idx < MAX_WIN)) || (state == ST_SUM);

    wire [HALF_SYMBOL_LOG2-1:0] dly_waddr  = clearing ? clr_idx[HALF_SYMBOL_LOG2-1:0] : idx;
    wire [MAX_WIN_LOG2-1:0]     hist_waddr = clearing ? clr_idx[MAX_WIN_LOG2-1:0] : hidx;

    always @(posedge clk) begin
        if (dly_we) begin
            delay_mem[dly_waddr] <= clearing ? 32'd0 : {x_re, x_im};
            power_mem[dly_waddr] <= clearing ? 32'd0 : new_power[31:0];
        end
        if (hist_we)
            hist_mem[hist_waddr] <= clearing ? 32'd0 : metric;

        // Free-running reads. idx/hidx only move on the edge that writes
        // their old slot, and each is stable for well over one cycle before
        // the state that consumes the read (ST_ACC / ST_SUM) comes around.
        delay_rd <= delay_mem[idx];
        power_rd <= power_mem[idx];
        hist_rd  <= hist_mem[hidx];
    end

    //--------------------------------------------------------------------------
    // Control FSM
    //--------------------------------------------------------------------------

    always @(posedge clk) begin
        if (rst) begin
            state   <= ST_CLEAR;
            clr_idx <= {(CLR_LOG2+1){1'b0}};
            idx     <= {HALF_SYMBOL_LOG2{1'b0}};
            hidx    <= {MAX_WIN_LOG2{1'b0}};

            if (cfg_averaging_window == 32'd0) begin
                win_n              <= {{MAX_WIN_LOG2{1'b0}}, 1'b1};
                cfg_window_clamped <= 1'b0;
            end else if (cfg_averaging_window > MAX_WIN) begin
                win_n              <= MAX_WIN[MAX_WIN_LOG2:0];
                cfg_window_clamped <= 1'b1;
            end else begin
                win_n              <= cfg_averaging_window[MAX_WIN_LOG2:0];
                cfg_window_clamped <= 1'b0;
            end

            x_re <= 16'sd0;
            x_im <= 16'sd0;
            p_re <= 64'sd0;
            p_im <= 64'sd0;
            r_sum <= 64'sd0;
            sample_count <= 48'd0;
            p_re_neg <= 1'b0;
            p_im_neg <= 1'b0;
            norm_re <= 64'sd0;
            norm_im <= 64'sd0;
            shift <= 6'd0;
            angle <= 16'sd0;
            magnitude <= 32'd0;
            metric <= 32'd0;
            metric_sum <= 64'd0;
            smoothed <= 64'd0;
            in_det <= 1'b0;
            peak_metric <= 32'd0;
            peak_sample <= 48'd0;
            peak_angle <= 16'sd0;

            div_start <= 1'b0;
            div_avg <= 1'b0;
            div_a_dividend <= 64'd0;
            div_a_divisor <= 64'd0;
            cordic_start <= 1'b0;

            s_axis_tready <= 1'b0;
            m_axis_tvalid <= 1'b0;
            m_axis_tdata  <= {`BOOTSTRAP_DETECTION_WIDTH{1'b0}};
            m_axis_tlast  <= 1'b0;
            mon_valid  <= 1'b0;
            mon_metric <= 32'd0;
            mon_cfo_hz <= 32'sd0;
        end else begin
            div_start    <= 1'b0;
            cordic_start <= 1'b0;
            mon_valid    <= 1'b0;

            case (state)
                ST_CLEAR: begin
                    if (clr_idx == CLR_LAST[CLR_LOG2:0]) begin
                        state         <= ST_IDLE;
                        s_axis_tready <= 1'b1;
                    end
                    clr_idx <= clr_idx + 1'b1;
                end

                ST_IDLE: begin
                    if (s_axis_tvalid) begin
                        s_axis_tready <= 1'b0;
                        x_re  <= s_axis_tdata[31:16];
                        x_im  <= s_axis_tdata[15:0];
                        state <= ST_ACC;
                    end
                end

                ST_ACC: begin
                    // delay_rd/power_rd were read on the accept edge.
                    p_re  <= p_re_next;
                    p_im  <= p_im_next;
                    r_sum <= r_next;
                    idx   <= idx + 1'b1;  // wraps at HALF_SYMBOL by width
                    sample_count <= sample_count + 48'd1;
                    state <= ST_DIV_START;
                end

                ST_DIV_START: begin
                    p_re_neg       <= p_re[63];
                    p_im_neg       <= p_im[63];
                    div_avg        <= 1'b0;
                    div_a_dividend <= p_re_abs <<< 15;
                    div_a_divisor  <= r_sum;
                    div_start      <= 1'b1;
                    state          <= ST_DIV_WAIT;
                end

                ST_DIV_WAIT: begin
                    // Both dividers start together with the same width, so
                    // they finish on the same cycle.
                    if (div_a_done) begin
                        norm_re <= p_re_neg ? -quo_a : quo_a;
                        norm_im <= p_im_neg ? -quo_b : quo_b;
                        shift   <= 6'd0;
                        state   <= ST_SHIFT;
                    end
                end

                ST_SHIFT: begin
                    if (norm_out_of_range && (shift < 6'd32)) begin
                        norm_re <= norm_re >>> 1;
                        norm_im <= norm_im >>> 1;
                        shift   <= shift + 6'd1;
                    end else begin
                        state <= ST_CORDIC_START;
                    end
                end

                ST_CORDIC_START: begin
                    cordic_start <= 1'b1;
                    state        <= ST_CORDIC_WAIT;
                end

                ST_CORDIC_WAIT: begin
                    if (cordic_done) begin
                        angle     <= cordic_angle;
                        magnitude <= cordic_mag;
                        state     <= ST_METRIC;
                    end
                end

                ST_METRIC: begin
                    metric <= metric_next;
                    state  <= ST_SUM;
                end

                ST_SUM: begin
                    // hist_mem[hidx] <= metric happens in the RAM block.
                    metric_sum     <= sum_next;
                    hidx           <= ({1'b0, hidx} == win_n - 1'b1) ? {MAX_WIN_LOG2{1'b0}}
                                                                     : hidx + 1'b1;
                    div_avg        <= 1'b1;
                    div_a_dividend <= sum_next;
                    div_a_divisor  <= {{(63-MAX_WIN_LOG2){1'b0}}, win_n};
                    div_start      <= 1'b1;
                    state          <= ST_AVG_WAIT;
                end

                ST_AVG_WAIT: begin
                    if (div_a_done) begin
                        smoothed <= div_a_quotient;
                        state    <= ST_DETECT;
                    end
                end

                ST_DETECT: begin
                    mon_valid  <= 1'b1;
                    mon_metric <= smoothed[31:0];
                    mon_cfo_hz <= cfo_shr_cur[31:0];

                    state         <= ST_IDLE;
                    s_axis_tready <= 1'b1;

                    if (!in_det) begin
                        if (smoothed_s > threshold) begin
                            in_det      <= 1'b1;
                            peak_metric <= smoothed[31:0];
                            peak_sample <= sample_count;
                            peak_angle  <= angle;
                        end
                    end else begin
                        peak_metric <= peak_next;
                        peak_sample <= peak_s_next;
                        peak_angle  <= peak_a_next;

                        if (falling_edge) begin
                            in_det      <= 1'b0;
                            peak_metric <= 32'd0;

                            m_axis_tdata <= {`BOOTSTRAP_DETECTION_WIDTH{1'b0}};
                            m_axis_tdata[`BOOTSTRAP_DETECTION_DETECTED_BIT] <= 1'b1;
                            m_axis_tdata[`BOOTSTRAP_DETECTION_SAMPLE_INDEX_HI:
                                         `BOOTSTRAP_DETECTION_SAMPLE_INDEX_LO] <= peak_s_next;
                            m_axis_tdata[`BOOTSTRAP_DETECTION_CFO_HZ_HI:
                                         `BOOTSTRAP_DETECTION_CFO_HZ_LO] <=
                                cfo_shr_peak[31:0];
                            m_axis_tdata[`BOOTSTRAP_DETECTION_METRIC_HI:
                                         `BOOTSTRAP_DETECTION_METRIC_LO] <= peak_next;
                            m_axis_tvalid <= 1'b1;
                            m_axis_tlast  <= 1'b1;

                            state         <= ST_EMIT;
                            s_axis_tready <= 1'b0;
                        end
                    end
                end

                ST_EMIT: begin
                    if (m_axis_tready) begin
                        m_axis_tvalid <= 1'b0;
                        m_axis_tlast  <= 1'b0;
                        state         <= ST_IDLE;
                        s_axis_tready <= 1'b1;
                    end
                end

                default: state <= ST_CLEAR;
            endcase
        end
    end

    // Unused by design: the golden model has no input framing; the busy
    // flags are implied by the FSM sequencing the done pulses; the low
    // bits of mag_sq are the >> 15 and the high bits of the CFO products
    // are sign bits of a value known to fit int32.
    wire unused_ok = &{1'b0, s_axis_tlast, div_a_busy, div_b_busy, div_b_done,
                       cordic_busy, mag_sq[14:0], cfo_shr_cur[48:32],
                       cfo_shr_peak[48:32], 1'b0};

endmodule
