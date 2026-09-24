// bootstrap_detector.v — ATSC 3.0 bootstrap detection + coarse CFO estimate
//
// Bit-exact port of lib/sync/bootstrap_detector.cc's ATSC3_FIXED_POINT path
// (Schmidl-Cox autocorrelation at lag L = HALF_SYMBOL = 2048). Per input
// sample, in the same order as process_sample() then check_detection():
//
//   1. Two EWMAs with alpha = 1/1024 (single-pole accumulators, not
//      2048-tap delay lines):
//        P: p = p - (p >>> 10) + 2 * x[n] * conj(x[n-L])
//        R: r = r - (r >>> 10) + |x[n]|^2 + |x[n-L]|^2
//      Identical weights make |P| <= R (Cauchy-Schwarz), so the metric is
//      bounded by 1 and a noise-to-signal edge cannot fake a correlation.
//   2. If R is below the near-silence floor (2^16), the normalized vector
//      is (0, 0) and the divides are skipped. Otherwise
//      norm = saturate16((P * 32768) / R) per rail, C++ truncate-toward-zero
//      division.
//   3. Shared CORDIC core (hdl/rtl/common/cordic.v), vectoring mode:
//      magnitude and angle of the normalized P.
//   4. metric = magnitude^2 >> 15 (<= 65536: magnitude <= ~46341).
//   5. Moving average of metric over the averaging window (history RAM +
//      running sum), divided by the window length.
//   6. Detection FSM: arm when smoothed > threshold, track the peak, fire
//      on the falling edge below 0.8 * peak (26214 in Q1.15, which is
//      float_to_q15(0.8f)'s truncated value), then stay disarmed until
//      smoothed <= threshold (one detection per bootstrap).
//
// Every arithmetic width mirrors the C++ int64_t/int32_t/int16_t choices
// directly rather than a tightened width, same policy as cordic.v; width
// optimization is timing-closure work.
//
// Throughput: this is a sequential datapath (two parallel 64-cycle
// divides, the iterative CORDIC, a 64-cycle averaging divide), roughly
// 150 clock cycles per input sample. At the 100 MHz nominal clock that is
// well below the 6.25 MS/s sample rate. Correctness first; faster dividers
// and a pipelined datapath are the timing-closure work tracked in
// TASKS.md. s_axis_tready backpressures the input while a sample is in
// flight.
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
                     ST_CORDIC_START = 4'd5,
                     ST_CORDIC_WAIT  = 4'd6,
                     ST_METRIC       = 4'd7,
                     ST_SUM          = 4'd8,
                     ST_AVG_WAIT     = 4'd9,
                     ST_DETECT       = 4'd10,
                     ST_EMIT         = 4'd11;

    // float_to_q15(0.8f), truncated, as in check_detection()'s falling edge
    localparam signed [63:0] FALLING_Q15 = 64'sd26214;
    // kMinCorrelatorEnergyQ30: near-silence floor on R
    localparam signed [63:0] MIN_ENERGY  = 64'sd65536;

    reg [3:0] state;

    //--------------------------------------------------------------------------
    // RAMs: delay line x[n-L] and the metric history. Plain inferred
    // synchronous-read memories; zeroed by the post-reset clear sweep.
    //--------------------------------------------------------------------------

    reg [31:0] delay_mem [0:HALF_SYMBOL-1];
    reg [31:0] hist_mem  [0:MAX_WIN-1];

    reg [31:0] delay_rd;
    reg [31:0] hist_rd;

    reg [CLR_LOG2:0]           clr_idx;
    reg [HALF_SYMBOL_LOG2-1:0] idx;   // delay_idx_
    reg [MAX_WIN_LOG2-1:0]     hidx;  // metric_idx_
    reg [MAX_WIN_LOG2:0]       win_n; // effective window, 1..MAX_WIN

    //--------------------------------------------------------------------------
    // Datapath state
    //--------------------------------------------------------------------------

    reg signed [15:0] x_re, x_im;
    reg signed [63:0] p_re, p_im;   // p_sum_re_/p_sum_im_
    reg signed [63:0] r_sum;        // r_sum_, always >= 0
    reg [47:0]        sample_count; // status-word width (see status_words.vh)

    reg               p_re_neg, p_im_neg;
    reg signed [15:0] norm_re, norm_im;

    reg signed [15:0] angle;
    reg [31:0]        magnitude;
    reg [31:0]        metric;
    reg [63:0]        metric_sum;  // sum of hist_mem, always >= 0
    reg [63:0]        smoothed;

    reg               in_det;
    reg               rearm_blocked;
    reg [31:0]        peak_metric;
    reg [47:0]        peak_sample;
    reg signed [15:0] peak_angle;

    //--------------------------------------------------------------------------
    // Step 1: correlation/energy update (ST_ACC)
    //--------------------------------------------------------------------------

    wire signed [15:0] xd_re = delay_rd[31:16];
    wire signed [15:0] xd_im = delay_rd[15:0];

    // Each 16x16 signed product fits int32 (max (-32768)^2 = 2^30); sums
    // are formed at 64 bits like the C++ int64_t locals.
    wire signed [31:0] m_rr   = x_re * xd_re;
    wire signed [31:0] m_ii   = x_im * xd_im;
    wire signed [31:0] m_ir   = x_im * xd_re;
    wire signed [31:0] m_ri   = x_re * xd_im;
    wire signed [31:0] m_xx_r = x_re * x_re;
    wire signed [31:0] m_xx_i = x_im * x_im;
    wire signed [31:0] m_dd_r = xd_re * xd_re;
    wire signed [31:0] m_dd_i = xd_im * xd_im;

    wire signed [63:0] corr_re = {{32{m_rr[31]}}, m_rr} + {{32{m_ii[31]}}, m_ii};
    wire signed [63:0] corr_im = {{32{m_ir[31]}}, m_ir} - {{32{m_ri[31]}}, m_ri};
    wire signed [63:0] energy  = {{32{m_xx_r[31]}}, m_xx_r} + {{32{m_xx_i[31]}}, m_xx_i} +
                                 {{32{m_dd_r[31]}}, m_dd_r} + {{32{m_dd_i[31]}}, m_dd_i};

    wire signed [63:0] p_re_next = p_re - (p_re >>> 10) + (corr_re <<< 1);
    wire signed [63:0] p_im_next = p_im - (p_im >>> 10) + (corr_im <<< 1);
    wire signed [63:0] r_next    = r_sum - (r_sum >>> 10) + energy;

    //--------------------------------------------------------------------------
    // Step 2: (P * 32768) / R, truncating toward zero: divide magnitudes,
    // restore the sign. |P| < 2^44 by the EWMA's bound, so |P| << 15 fits.
    //--------------------------------------------------------------------------

    wire energetic = (r_sum >= MIN_ENERGY);

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

    wire signed [63:0] quo_a  = div_a_quotient;
    wire signed [63:0] quo_b  = div_b_quotient;
    wire signed [63:0] q_re_s = p_re_neg ? -quo_a : quo_a;
    wire signed [63:0] q_im_s = p_im_neg ? -quo_b : quo_b;

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
        .in_a  (norm_re),
        .in_b  (norm_im),
        .busy  (cordic_busy),
        .done  (cordic_done),
        .out_a (cordic_angle),
        .out_b (cordic_mag)
    );

    //--------------------------------------------------------------------------
    // Step 4/5: metric and its moving average
    //--------------------------------------------------------------------------

    // (int64 magnitude * magnitude) >> 15, truncated to int32 as in C++.
    // magnitude is never negative (CORDIC vectoring output).
    wire [63:0] mag_sq      = {32'd0, magnitude} * {32'd0, magnitude};
    wire [31:0] metric_next = mag_sq[46:15];

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
        if (dly_we)
            delay_mem[dly_waddr] <= clearing ? 32'd0 : {x_re, x_im};
        if (hist_we)
            hist_mem[hist_waddr] <= clearing ? 32'd0 : metric;

        // Free-running reads. idx/hidx only move on the edge that writes
        // their old slot, and each is stable for well over one cycle before
        // the state that consumes the read (ST_ACC / ST_SUM) comes around.
        delay_rd <= delay_mem[idx];
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
            norm_re <= 16'sd0;
            norm_im <= 16'sd0;
            angle <= 16'sd0;
            magnitude <= 32'd0;
            metric <= 32'd0;
            metric_sum <= 64'd0;
            smoothed <= 64'd0;
            in_det <= 1'b0;
            rearm_blocked <= 1'b0;
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
                    // delay_rd was read on the accept edge.
                    p_re  <= p_re_next;
                    p_im  <= p_im_next;
                    r_sum <= r_next;
                    idx   <= idx + 1'b1;  // wraps at HALF_SYMBOL by width
                    sample_count <= sample_count + 48'd1;
                    state <= ST_DIV_START;
                end

                ST_DIV_START: begin
                    if (energetic) begin
                        p_re_neg       <= p_re[63];
                        p_im_neg       <= p_im[63];
                        div_avg        <= 1'b0;
                        div_a_dividend <= p_re_abs <<< 15;
                        div_a_divisor  <= r_sum;
                        div_start      <= 1'b1;
                        state          <= ST_DIV_WAIT;
                    end else begin
                        // Below the energy floor: vector (0, 0), so CORDIC
                        // yields magnitude 0 and angle 0 (metric 0).
                        norm_re <= 16'sd0;
                        norm_im <= 16'sd0;
                        state   <= ST_CORDIC_START;
                    end
                end

                ST_DIV_WAIT: begin
                    // Both dividers start together with the same width, so
                    // they finish on the same cycle.
                    if (div_a_done) begin
                        norm_re <= saturate_i16(q_re_s);
                        norm_im <= saturate_i16(q_im_s);
                        state   <= ST_CORDIC_START;
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
                        if (rearm_blocked) begin
                            if (smoothed_s <= threshold)
                                rearm_blocked <= 1'b0;
                        end else if (smoothed_s > threshold) begin
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
                            in_det        <= 1'b0;
                            rearm_blocked <= 1'b1;
                            peak_metric   <= 32'd0;

                            m_axis_tdata <= {`BOOTSTRAP_DETECTION_WIDTH{1'b0}};
                            m_axis_tdata[`BOOTSTRAP_DETECTION_DETECTED_BIT] <= 1'b1;
                            m_axis_tdata[`BOOTSTRAP_DETECTION_SAMPLE_INDEX_HI:
                                         `BOOTSTRAP_DETECTION_SAMPLE_INDEX_LO] <= peak_s_next;
                            m_axis_tdata[`BOOTSTRAP_DETECTION_CFO_HZ_HI:
                                         `BOOTSTRAP_DETECTION_CFO_HZ_LO] <= cfo_shr_peak[31:0];
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
    // flags are implied by the FSM sequencing the done pulses; mag_sq's
    // low bits are the >> 15 and its high bits are the int32 truncation;
    // the CFO products' high bits are sign bits of a value known to fit
    // int32.
    wire unused_ok = &{1'b0, s_axis_tlast, div_a_busy, div_b_busy, div_b_done,
                       cordic_busy, mag_sq[14:0], mag_sq[63:47], cfo_shr_cur[48:32],
                       cfo_shr_peak[48:32], 1'b0};

endmodule
