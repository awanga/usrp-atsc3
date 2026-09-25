// timing_recovery.v — Symbol timing recovery: polyphase interpolator +
// Gardner TED + second-order loop filter
//
// Bit-exact port of lib/sync/timing_recovery.cc's ATSC3_FIXED_POINT path
// (TimingRecovery::process_sample()). Every input sample:
//   1. Written into a 128-entry circular sample buffer (BUF_SIZE =
//      4 * taps_per_phase, matching the C++ constructor).
//   2. samples_since_symbol counts up; once it reaches
//      cfg_samples_per_symbol (2 by default, 2x oversampling), that
//      counter resets to 0 and a *symbol boundary* fires:
//        a. If the buffer has >= NUM_TAPS samples, interpolate at
//           buf_read_idx (a separate pointer, advanced by
//           samples_per_symbol only at boundaries -- see
//           lib/sync/timing_recovery.cc's process_sample() for why this
//           tracks buf_write_idx at a roughly constant lag) and emit
//           that as one m_axis beat. No TLAST: each beat already is one
//           complete symbol-rate sample, there is no further grouping at
//           this stage (matches the software AXI4-S comment: symbol-rate
//           samples, aligned).
//        b. If additionally cfg_locked, run the Gardner TED: interpolate
//           three more points (curr = the sample just written, mid =
//           curr - samples_per_symbol/2, prev = curr -
//           samples_per_symbol, mid interpolated at mu+0.5) and feed
//           GardnerTed::compute_error()'s formula into the second-order
//           loop filter, updating loop_integrator_q15/mu_q16/
//           timing_error_q15.
//        c. buf_read_idx advances by samples_per_symbol.
//   3. mon_valid pulses once per accepted input sample (matching
//      get_timing_offset()/get_timing_error(), which the C++ updates
//      unconditionally in check_detection()'s update_loop() -- actually
//      these are read directly from mu_q16_/timing_error_q15_, which
//      only change on a boundary+locked update; mon_* simply mirrors
//      whatever the registers hold after this sample, same as the golden
//      CLI's "M" line).
//
// Up to four interpolate() calls happen per boundary (emit, curr, mid,
// prev), each ~70 cycles through the shared polyphase_fir core (see that
// file's header) -- not pipelined, correctness first (9.17 timing-
// closure work), the same policy bootstrap_detector.v documents.
//
// Config (kp_q15, ki_q15, samples_per_symbol, initial_offset) is
// validated -- there is nothing to clamp, these are host-supplied
// literal Q-format/count values -- and LATCHED at reset, the same
// cp_removal.v/bootstrap_detector.v pattern (mid-stream reconfiguration
// isn't a golden-model behavior to match; the C++ only changes these via
// the constructor or reset()). kp_q15/ki_q15 are taken as *direct*
// registers rather than re-derived in RTL from loop_bandwidth_hz/
// loop_damping/symbol_rate_hz (compute_loop_gains()'s one-time,
// division-and-multiply-heavy filter design) -- see TASKS.md's 9.2
// implementation notes for the rationale; this is the RTL-port
// counterpart of the polyphase coefficient ROM also being computed
// offline rather than re-derived per cycle. cfg_locked is the one config
// input read *live*: set_locked() is an explicitly asynchronous runtime
// control in the C++ API (training vs. tracking mode), not a
// reconfigure-only value.
//
// mu_q16 (Q0.16 unsigned, wraps to [0,1) for free via plain unsigned
// arithmetic) is initialized at reset from cfg_initial_offset_q15 (a
// Q1.15 value) via a left shift by one bit: reinterpreting a 16-bit
// twos-complement pattern as unsigned and doubling it is bit-for-bit
// identical to the C++'s set_timing_offset()'s
// "wrapped = mu - floor(mu); mu_q16_ = uint16_t(wrapped * 65536)" for
// every possible 16-bit input, not just non-negative ones (worked
// through by hand in the commit history; both paths ultimately compute
// (v mod 65536) * 2 mod 65536).
//
// Phase selection (0..15 from mu_q16, and mu_q16+0.5 for the TED's
// mid-point) is just the top 4 bits of mu_q16 (mu_q16[15:12], or
// (mu_q16 + 16'h8000)[15:12] for the +0.5 case) -- exact because
// num_phases = 16 is a power of 2: floor(mu * 16) in double, for
// mu = mu_q16/65536.0, is exactly mu_q16 >> 12 (no rounding case to
// diverge on).
//
// AXI4-S: TDATA=ci16 in and out. No TLAST on either side (see point 2a
// above; the golden model has no input framing either, matching
// bootstrap_detector.v/cp_removal.v).

`include "axi4s_types.vh"

module timing_recovery #(
    parameter NUM_PHASES     = 16,
    parameter NUM_TAPS       = 32,
    parameter PHASE_WIDTH    = 4,
    parameter BUF_ADDR_WIDTH = 7,   // BUF_SIZE = 128 = 4 * NUM_TAPS
    parameter BUF_SIZE       = 128
) (
    input  wire                clk,
    input  wire                rst,  // synchronous, active-high

    input  wire signed [15:0]  cfg_kp_q15,
    input  wire signed [15:0]  cfg_ki_q15,
    input  wire [7:0]          cfg_samples_per_symbol,
    input  wire signed [15:0]  cfg_initial_offset_q15,
    input  wire                cfg_locked,  // read live -- see header comment

    `AXI4S_SLAVE(s_axis, `ATSC3_SAMPLE_WIDTH),
    `AXI4S_MASTER(m_axis, `ATSC3_SAMPLE_WIDTH),

    output reg                 mon_valid,
    output reg  [15:0]         mon_mu_q16,
    output reg  signed [63:0]  mon_timing_error_q15
);

    //--------------------------------------------------------------------
    // Sample buffer RAM: BUF_SIZE entries, {re,im} packed like TDATA.
    //--------------------------------------------------------------------

    reg [31:0] buf_mem [0:BUF_SIZE-1];

    reg                       buf_wr_en;
    reg  [BUF_ADDR_WIDTH-1:0] buf_wr_addr;
    reg  [31:0]               buf_wr_data;
    reg  [BUF_ADDR_WIDTH-1:0] fir_mem_addr;
    wire signed [15:0]        fir_mem_data_re = buf_mem[fir_mem_addr][31:16];
    wire signed [15:0]        fir_mem_data_im = buf_mem[fir_mem_addr][15:0];

    always @(posedge clk) begin
        if (buf_wr_en) begin
            buf_mem[buf_wr_addr] <= buf_wr_data;
        end
    end

    //--------------------------------------------------------------------
    // Shared polyphase FIR core, invoked sequentially up to 4x/boundary.
    //--------------------------------------------------------------------

    reg                       fir_start;
    reg  [PHASE_WIDTH-1:0]    fir_phase;
    reg  [BUF_ADDR_WIDTH-1:0] fir_base_idx;
    wire                      fir_busy, fir_done;
    wire signed [15:0]        fir_result_re, fir_result_im;

    polyphase_fir #(
        .NUM_PHASES     (NUM_PHASES),
        .NUM_TAPS       (NUM_TAPS),
        .PHASE_WIDTH    (PHASE_WIDTH),
        .BUF_ADDR_WIDTH (BUF_ADDR_WIDTH)
    ) u_fir (
        .clk          (clk),
        .rst          (rst),
        .start        (fir_start),
        .phase        (fir_phase),
        .base_idx     (fir_base_idx),
        .mem_addr     (fir_mem_addr),
        .mem_data_re  (fir_mem_data_re),
        .mem_data_im  (fir_mem_data_im),
        .busy         (fir_busy),
        .done         (fir_done),
        .result_re    (fir_result_re),
        .result_im    (fir_result_im)
    );

    //--------------------------------------------------------------------
    // Latched-at-reset config (see header comment)
    //--------------------------------------------------------------------

    reg signed [15:0] kp_q15_r, ki_q15_r;
    reg [7:0]          sps_r;

    //--------------------------------------------------------------------
    // Persistent state (mirrors TimingRecovery's members 1:1)
    //--------------------------------------------------------------------

    reg [BUF_ADDR_WIDTH-1:0] buf_write_idx, buf_read_idx;
    reg [8:0]                 buf_count;   // 0..BUF_SIZE (needs one bit more than BUF_ADDR_WIDTH)
    reg [7:0]                 samples_since_symbol;

    reg [15:0]                mu_q16;
    reg signed [63:0]         timing_error_q15;
    reg signed [63:0]         loop_integrator_q15;

    //--------------------------------------------------------------------
    // Per-boundary scratch state
    //--------------------------------------------------------------------

    reg [15:0]                mu_snapshot;
    reg [BUF_ADDR_WIDTH-1:0]  curr_idx, mid_idx, prev_idx;
    reg                       emit_valid;      // buf_count >= NUM_TAPS at this boundary
    reg signed [15:0]         emit_re, emit_im;
    reg signed [15:0]         x_curr_re, x_curr_im;
    reg signed [15:0]         x_mid_re, x_mid_im;
    reg signed [15:0]         x_prev_re, x_prev_im;
    reg signed [63:0]         error_q15;

    localparam [BUF_ADDR_WIDTH-1:0] NUM_TAPS_ADDR = NUM_TAPS[BUF_ADDR_WIDTH-1:0];

    wire [BUF_ADDR_WIDTH-1:0] half_sps = sps_r[7:1];  // sps_r / 2, C++ integer division
    wire [BUF_ADDR_WIDTH-1:0] full_sps = sps_r[BUF_ADDR_WIDTH-1:0];

    // mu_snapshot + 0.5, wrapped to [0,1) via plain 16-bit unsigned
    // overflow (see header comment on the mu+0.5 trick). Only the top 4
    // bits (the phase select) are ever read -- see unused_ok below.
    wire [15:0] mu_snapshot_plus_half = mu_snapshot + 16'h8000;

    //--------------------------------------------------------------------
    // Control FSM
    //--------------------------------------------------------------------

    // Each "start a FIR call" step is fused into the state that decides
    // to make the call (ST_EMIT_DECIDE, ST_TED_DECIDE) or that consumes
    // the previous call's result and immediately issues the next
    // (ST_TED_CURR_WAIT -> mid, ST_TED_MID_WAIT -> prev) -- there is no
    // separate "_START" state for any of the four calls.
    localparam [3:0] ST_CLEAR         = 4'd0,
                     ST_IDLE          = 4'd1,
                     ST_WRITE         = 4'd2,
                     ST_CHECK         = 4'd3,
                     ST_EMIT_DECIDE   = 4'd4,
                     ST_EMIT_WAIT     = 4'd5,
                     ST_TED_DECIDE    = 4'd6,
                     ST_TED_CURR_WAIT = 4'd7,
                     ST_TED_MID_WAIT  = 4'd8,
                     ST_TED_PREV_WAIT = 4'd9,
                     ST_COMPUTE_ERROR = 4'd10,
                     ST_UPDATE_LOOP   = 4'd11,
                     ST_ADVANCE_READ  = 4'd12,
                     ST_OUTPUT        = 4'd13,
                     ST_DONE_SAMPLE   = 4'd14;

    reg [3:0]                 state;
    reg [BUF_ADDR_WIDTH-1:0]  clr_idx;
    reg signed [15:0]         x_re, x_im;

    // Loop-filter combinational terms (ST_UPDATE_LOOP)
    wire signed [63:0] ki_term = ($signed({{48{ki_q15_r[15]}}, ki_q15_r}) * error_q15) >>> 15;
    wire signed [63:0] new_loop_integrator = loop_integrator_q15 + ki_term;
    wire signed [63:0] kp_term = ($signed({{48{kp_q15_r[15]}}, kp_q15_r}) * error_q15) >>> 15;
    wire signed [63:0] adjustment_q15 = kp_term + new_loop_integrator;
    // Only the low 16 bits of (adjustment_q15 << 1) ever matter -- the
    // C++ adds the full-width value to a uint16_t and lets the implicit
    // truncation take care of the rest, and truncation mod 2^16
    // commutes with addition, so computing just those 16 bits directly
    // (instead of a 64-bit shift and then slicing) is exactly equivalent
    // and avoids carrying 48 bits nothing ever reads.
    wire [15:0] adjustment_q16 = {adjustment_q15[14:0], 1'b0};
    wire [15:0] new_mu_q16 = mu_q16 + adjustment_q16;
    // timing_error_q15 = (timing_error_q15*0.9 + error_q15*0.1), exact Q1.15 gains
    wire signed [63:0] filt_a = timing_error_q15 * 64'sd29491;  // float_to_q15(0.9f)
    wire signed [63:0] filt_b = error_q15 * 64'sd3277;          // float_to_q15(0.1f)
    wire signed [63:0] new_timing_error_q15 = (filt_a + filt_b) >>> 15;

    // ST_COMPUTE_ERROR combinational terms
    // Plain signed subtraction: both operands are already `reg signed`,
    // so Verilog's context-determined evaluation sign-extends each to
    // the 17-bit result width before subtracting -- correctly, unlike
    // an earlier version of this line that zero-extended them by hand
    // (prepending a literal 1'b0), which silently reinterpreted any
    // negative x_curr_re/x_prev_re as a large positive value and
    // corrupted the Gardner TED error on real (signed) samples. Caught
    // by test_timing_recovery.py's locked-loop scenarios: mu_q16
    // diverged from the golden model starting at the very first
    // loop-filter update.
    wire signed [16:0] diff_re = x_curr_re - x_prev_re;
    wire signed [16:0] diff_im = x_curr_im - x_prev_im;
    wire signed [63:0] error_raw =
        $signed(diff_re) * $signed({{48{x_mid_re[15]}}, x_mid_re}) +
        $signed(diff_im) * $signed({{48{x_mid_im[15]}}, x_mid_im});

    always @(posedge clk) begin
        if (rst) begin
            state   <= ST_CLEAR;
            clr_idx <= {BUF_ADDR_WIDTH{1'b0}};

            kp_q15_r <= cfg_kp_q15;
            ki_q15_r <= cfg_ki_q15;
            sps_r    <= cfg_samples_per_symbol;
            // mu_q16 = (cfg_initial_offset_q15 reinterpreted unsigned) * 2,
            // mod 65536 -- see header comment.
            mu_q16   <= {cfg_initial_offset_q15[14:0], 1'b0};

            buf_write_idx        <= {BUF_ADDR_WIDTH{1'b0}};
            buf_read_idx         <= {BUF_ADDR_WIDTH{1'b0}};
            buf_count             <= 9'd0;
            samples_since_symbol <= 8'd0;

            timing_error_q15     <= 64'sd0;
            loop_integrator_q15  <= 64'sd0;

            buf_wr_en   <= 1'b0;
            buf_wr_addr <= {BUF_ADDR_WIDTH{1'b0}};
            buf_wr_data <= 32'd0;
            fir_start   <= 1'b0;

            s_axis_tready         <= 1'b0;
            m_axis_tvalid         <= 1'b0;
            m_axis_tdata          <= {`ATSC3_SAMPLE_WIDTH{1'b0}};
            m_axis_tlast          <= 1'b0;
            mon_valid             <= 1'b0;
            mon_mu_q16            <= 16'd0;
            mon_timing_error_q15  <= 64'sd0;
        end else begin
            buf_wr_en <= 1'b0;
            fir_start <= 1'b0;
            mon_valid <= 1'b0;

            case (state)
                ST_CLEAR: begin
                    buf_wr_en   <= 1'b1;
                    buf_wr_addr <= clr_idx;
                    buf_wr_data <= 32'd0;
                    if (clr_idx == {BUF_ADDR_WIDTH{1'b1}}) begin
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
                        state <= ST_WRITE;
                    end
                end

                ST_WRITE: begin
                    buf_wr_en   <= 1'b1;
                    buf_wr_addr <= buf_write_idx;
                    buf_wr_data <= {x_re, x_im};

                    buf_write_idx <= buf_write_idx + 1'b1;
                    if (buf_count < BUF_SIZE[8:0]) begin
                        buf_count <= buf_count + 1'b1;
                    end
                    samples_since_symbol <= samples_since_symbol + 1'b1;
                    state <= ST_CHECK;
                end

                ST_CHECK: begin
                    // samples_since_symbol already reflects ST_WRITE's
                    // increment by the time this cycle executes (both
                    // took effect on the same clock edge) -- no further
                    // +1 here.
                    if (samples_since_symbol >= sps_r) begin
                        samples_since_symbol <= 8'd0;

                        mu_snapshot <= mu_q16;
                        // buf_write_idx already reflects the post-increment
                        // value from ST_WRITE, so "-1" is the slot just
                        // written -- matches process_sample()'s curr_idx.
                        curr_idx <= buf_write_idx - 1'b1;
                        mid_idx  <= buf_write_idx - 1'b1 - half_sps;
                        prev_idx <= buf_write_idx - 1'b1 - full_sps;

                        emit_valid <= (buf_count >= {{(9-BUF_ADDR_WIDTH){1'b0}}, NUM_TAPS_ADDR});
                        state <= ST_EMIT_DECIDE;
                    end else begin
                        state <= ST_DONE_SAMPLE;
                    end
                end

                ST_EMIT_DECIDE: begin
                    if (emit_valid) begin
                        fir_start    <= 1'b1;
                        fir_phase    <= mu_snapshot[15:12];
                        fir_base_idx <= buf_read_idx;
                        state        <= ST_EMIT_WAIT;
                    end else begin
                        state <= ST_TED_DECIDE;
                    end
                end

                ST_EMIT_WAIT: begin
                    if (fir_done) begin
                        emit_re <= fir_result_re;
                        emit_im <= fir_result_im;
                        state   <= ST_TED_DECIDE;
                    end
                end

                ST_TED_DECIDE: begin
                    if (emit_valid && cfg_locked) begin
                        fir_start    <= 1'b1;
                        fir_phase    <= mu_snapshot[15:12];
                        fir_base_idx <= curr_idx;
                        state        <= ST_TED_CURR_WAIT;
                    end else begin
                        state <= ST_ADVANCE_READ;
                    end
                end

                ST_TED_CURR_WAIT: begin
                    if (fir_done) begin
                        x_curr_re    <= fir_result_re;
                        x_curr_im    <= fir_result_im;
                        fir_start    <= 1'b1;
                        fir_phase    <= mu_snapshot_plus_half[15:12];
                        fir_base_idx <= mid_idx;
                        state        <= ST_TED_MID_WAIT;
                    end
                end

                ST_TED_MID_WAIT: begin
                    if (fir_done) begin
                        x_mid_re     <= fir_result_re;
                        x_mid_im     <= fir_result_im;
                        fir_start    <= 1'b1;
                        fir_phase    <= mu_snapshot[15:12];
                        fir_base_idx <= prev_idx;
                        state        <= ST_TED_PREV_WAIT;
                    end
                end

                ST_TED_PREV_WAIT: begin
                    if (fir_done) begin
                        x_prev_re <= fir_result_re;
                        x_prev_im <= fir_result_im;
                        state     <= ST_COMPUTE_ERROR;
                    end
                end

                ST_COMPUTE_ERROR: begin
                    error_q15 <= error_raw >>> 15;
                    state     <= ST_UPDATE_LOOP;
                end

                ST_UPDATE_LOOP: begin
                    loop_integrator_q15 <= new_loop_integrator;
                    mu_q16              <= new_mu_q16;
                    timing_error_q15    <= new_timing_error_q15;
                    state               <= ST_ADVANCE_READ;
                end

                ST_ADVANCE_READ: begin
                    buf_read_idx <= buf_read_idx + full_sps;
                    if (emit_valid) begin
                        m_axis_tdata  <= {emit_re, emit_im};
                        m_axis_tvalid <= 1'b1;
                        state         <= ST_OUTPUT;
                    end else begin
                        state <= ST_DONE_SAMPLE;
                    end
                end

                ST_OUTPUT: begin
                    if (m_axis_tready) begin
                        m_axis_tvalid <= 1'b0;
                        state         <= ST_DONE_SAMPLE;
                    end
                end

                ST_DONE_SAMPLE: begin
                    mon_valid            <= 1'b1;
                    mon_mu_q16           <= mu_q16;
                    mon_timing_error_q15 <= timing_error_q15;
                    s_axis_tready        <= 1'b1;
                    state                <= ST_IDLE;
                end

                default: state <= ST_CLEAR;
            endcase
        end
    end

    // Unused by design: TLAST has no input framing to honor (see header);
    // fir_busy is implied by the FSM's own start/done sequencing;
    // cfg_initial_offset_q15's sign bit is deliberately dropped by the
    // mu_q16 reset derivation (see that comment); mu_snapshot_plus_half's
    // low 12 bits feed nothing but the phase-select slice.
    // adjustment_q15's high bits similarly feed nothing but
    // adjustment_q16's 15-bit slice (see that computation's comment).
    wire unused_ok = &{1'b0, s_axis_tlast, fir_busy, cfg_initial_offset_q15[15],
                       mu_snapshot_plus_half[11:0], adjustment_q15[63:15], 1'b0};

endmodule
