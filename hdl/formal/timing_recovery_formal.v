// timing_recovery_formal.v — Formal harness top for timing_recovery
//
// Verification-only. Not synthesized, not part of rtl/. Instantiates the
// flatten+expose-generated timing_recovery_bare (produced earlier in
// timing_recovery.sby's [script] -- see that file, timing_recovery_bare.v,
// and hdl/docs/formal_conventions.md for the two-stage flatten+expose
// flow this replaces a `bind`-based checker with).
//
// Scoped to exactly TASKS.md's 9.2 formal ask -- "phase accumulator wrap
// behavior, AXI4-S protocol properties" -- not an exhaustive proof of
// the whole 15-state control FSM or the polyphase FIR's arithmetic
// (cocotb's job, against the real golden model, at full width; see
// test_timing_recovery.py).
//
// Properties:
//   - FSM legality: \dut.state (exposed) is always one of the 15 defined
//     encodings (0..14), never the 1-bit-vector's other possible values.
//   - Phase accumulator wrap, reset-time derivation: mu_q16's reset
//     value is computed in timing_recovery.v as a single bit-slice trick
//     ({cfg_initial_offset_q15[14:0], 1'b0}), justified in that file's
//     header as bit-exact to the C++'s "reinterpret Q1.15 as unsigned,
//     multiply by 2, wrap mod 65536" for every possible 16-bit input.
//     This harness checks that claim against an *independently*
//     computed reference (an explicit wider add, truncated), not just
//     re-reading the DUT's own formula -- so it is a genuine
//     cross-check, not a self-consistency tautology.
//   - s_axis_tready / m_axis_tvalid each pinned to their exact
//     controlling FSM state (ST_IDLE / ST_OUTPUT). This is the form
//     k-induction actually needs to close: a purely temporal statement
//     of "tready never high right after an accept" or "tvalid stays
//     high while stalled" is true but not itself inductive, since
//     without an FSM-state anchor k-induction is free to consider an
//     unreachable predecessor (e.g. m_axis_tvalid=1 while state is
//     ST_CLEAR, which nothing in the RTL would ever clear, "vacuously"
//     satisfying naive stability). Pinning both signals to their state
//     first, then deriving stability/no-double-accept from that, closes
//     the proof; both weaker forms were tried first and failed
//     induction (not vacuously -- z3 found the ST_CLEAR-with-tvalid-set
//     counterexample directly).
//   - Standard AXI4-S output stability on m_axis while stalled
//     (mirroring axi4s_skid_buffer_formal.v/cp_removal_formal.v).
//
// Non-vacuity: as a sanity check (not part of the checked-in flow),
// reverting mu_q16's reset derivation to a plain pass-through of
// cfg_initial_offset_q15 (dropping the x2 wraparound) fails `prove`'s
// BMC base case immediately (step 2) on the phase-accumulator-wrap
// cross-check, confirming that property is genuinely load-bearing.
//
// Unlike bootstrap_detector.sby, plain `smtbmc z3` (mode prove, not
// PDR) closes this in a few seconds even with the polyphase FIR's and
// loop filter's multiplies cut -- there was no need to reach for
// prove_pdr.sh/ABC PDR here.
//
// Run: cd hdl/formal && sby -f timing_recovery.sby

`include "axi4s_types.vh"

module timing_recovery_formal (
    input wire clk,
    input wire rst
);

    reg  signed [15:0] cfg_kp_q15;
    reg  signed [15:0] cfg_ki_q15;
    reg  [7:0]          cfg_samples_per_symbol;
    reg  signed [15:0] cfg_initial_offset_q15;
    reg                 cfg_locked;

    reg  [31:0] s_axis_tdata;
    reg         s_axis_tvalid;
    wire        s_axis_tready;
    reg         s_axis_tlast;

    wire [31:0] m_axis_tdata;
    wire        m_axis_tvalid;
    reg         m_axis_tready;
    wire        m_axis_tlast;

    wire        mon_valid;
    wire [15:0] mon_mu_q16;
    wire signed [63:0] mon_timing_error_q15;

    // Probes wired to the DUT's internal state/mu_q16 registers via the
    // ports timing_recovery.sby's [script] exposed on
    // timing_recovery_bare -- see that file's header.
    wire [3:0]  state_probe;
    wire [15:0] mu_q16_probe;

    timing_recovery_bare dut_top (
        .clk                    (clk),
        .rst                    (rst),
        .cfg_kp_q15             (cfg_kp_q15),
        .cfg_ki_q15             (cfg_ki_q15),
        .cfg_samples_per_symbol (cfg_samples_per_symbol),
        .cfg_initial_offset_q15 (cfg_initial_offset_q15),
        .cfg_locked             (cfg_locked),
        .s_axis_tdata           (s_axis_tdata),
        .s_axis_tvalid          (s_axis_tvalid),
        .s_axis_tready          (s_axis_tready),
        .s_axis_tlast           (s_axis_tlast),
        .m_axis_tdata           (m_axis_tdata),
        .m_axis_tvalid          (m_axis_tvalid),
        .m_axis_tready          (m_axis_tready),
        .m_axis_tlast           (m_axis_tlast),
        .mon_valid              (mon_valid),
        .mon_mu_q16             (mon_mu_q16),
        .mon_timing_error_q15   (mon_timing_error_q15),
        .\dut.state  (state_probe),
        .\dut.mu_q16 (mu_q16_probe)
    );

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;

    always @(*) begin
        if (!past_valid) begin
            assume (rst);
        end
    end

    reg        prev_rst;
    reg signed [15:0] prev_cfg_initial_offset_q15;
    reg        prev_s_valid, prev_s_ready, prev_s_tlast;
    reg [31:0] prev_s_tdata;
    reg        prev_m_valid, prev_m_ready, prev_m_tlast;
    reg [31:0] prev_m_tdata;

    always @(posedge clk) begin
        prev_rst                     <= rst;
        prev_cfg_initial_offset_q15  <= cfg_initial_offset_q15;
        prev_s_valid                 <= s_axis_tvalid;
        prev_s_ready                 <= s_axis_tready;
        prev_s_tdata                 <= s_axis_tdata;
        prev_s_tlast                 <= s_axis_tlast;
        prev_m_valid                 <= m_axis_tvalid;
        prev_m_ready                 <= m_axis_tready;
        prev_m_tdata                 <= m_axis_tdata;
        prev_m_tlast                 <= m_axis_tlast;
    end

    // Standard AXI4-S producer-compliance assumption on s_axis, needed
    // only for the m_axis output-stability property below (same role as
    // in axi4s_skid_buffer_formal.v/cp_removal_formal.v).
    always @(*) begin
        if (past_valid && !prev_rst && prev_s_valid && !prev_s_ready) begin
            assume (s_axis_tvalid);
            assume (s_axis_tdata == prev_s_tdata);
            assume (s_axis_tlast == prev_s_tlast);
        end
    end

    //--------------------------------------------------------------------
    // FSM legality
    //--------------------------------------------------------------------
    always @(posedge clk) begin
        if (past_valid) begin
            assert (state_probe <= 4'd14);
        end
    end

    //--------------------------------------------------------------------
    // Phase accumulator wrap: independent cross-check of the reset-time
    // Q1.15 -> Q0.16 derivation (see file header). offset_unsigned_x2 is
    // computed via an explicit 17-bit add of the reinterpreted-unsigned
    // value with itself, truncated to 16 bits -- a different arithmetic
    // path than the DUT's bit-slice, that happens to compute the same
    // "multiply by 2 mod 65536" result if and only if the DUT's trick is
    // actually correct.
    //--------------------------------------------------------------------
    wire [15:0] offset_unsigned = prev_cfg_initial_offset_q15;
    wire [16:0] offset_unsigned_x2_wide = {1'b0, offset_unsigned} + {1'b0, offset_unsigned};

    always @(posedge clk) begin
        if (past_valid && prev_rst) begin
            assert (mu_q16_probe == offset_unsigned_x2_wide[15:0]);
        end
    end

    //--------------------------------------------------------------------
    // AXI4-S protocol
    //--------------------------------------------------------------------

    // m_axis_tvalid is high exactly in ST_OUTPUT (state == 4'd13) -- the
    // direct FSM-state restatement k-induction actually needs to close
    // (mirroring the s_axis_tready property below): without pinning
    // m_axis_tvalid to a state, k-induction is free to consider an
    // unreachable predecessor with m_axis_tvalid=1 in some other state
    // (e.g. ST_CLEAR) where nothing in the RTL would ever clear it, which
    // is vacuously "stable" but isn't the property this is meant to
    // establish. This implies output stability while stalled: nothing
    // touches m_axis_tdata/tlast except the ST_ADVANCE_READ->ST_OUTPUT
    // transition and the ST_OUTPUT-with-tready-accepted clear, so once
    // m_axis_tvalid is pinned to state==ST_OUTPUT, tdata/tlast staying
    // put for the cycles in between follows from the same argument.
    always @(posedge clk) begin
        if (past_valid && !prev_rst) begin
            assert (m_axis_tvalid == (state_probe == 4'd13));
        end
        if (past_valid && !prev_rst && prev_m_valid && !prev_m_ready) begin
            assert (m_axis_tdata == prev_m_tdata);
            assert (m_axis_tlast == prev_m_tlast);
        end
    end

    // s_axis_tready is high exactly in ST_IDLE (state == 4'd1) -- a
    // direct restatement of timing_recovery.v's own invariant (tready is
    // set only when entering ST_IDLE, from ST_CLEAR's last cycle or
    // ST_DONE_SAMPLE, and cleared only when leaving it to ST_WRITE),
    // which also rules out double-accepting a sample before the FSM
    // finishes the previous one. The weaker, purely-temporal version of
    // this property ("tready never high the cycle after an accept")
    // was not directly inductive on its own -- true, but k-induction
    // needs the FSM-state relationship spelled out, not just the
    // temporal symptom, to close the step.
    always @(posedge clk) begin
        if (past_valid && !prev_rst) begin
            assert (s_axis_tready == (state_probe == 4'd1));
        end
    end

    // Reset clears s_axis_tready/m_axis_tvalid.
    always @(posedge clk) begin
        if (past_valid && prev_rst) begin
            assert (!s_axis_tready);
            assert (!m_axis_tvalid);
        end
    end

    //--------------------------------------------------------------------
    // Non-vacuity: s_axis_tready / m_axis_tvalid / mon_valid are not
    // covered here -- reaching any of them needs the reset-time
    // ST_CLEAR sweep to finish first (128 cycles just to zero the
    // sample buffer, before ST_IDLE is even reachable), well past this
    // depth (20) and impractical to unroll, the same reasoning
    // cp_removal.sby's excluded cover goal documents. All three fire
    // repeatedly, concretely, in test_timing_recovery.py's cocotb suite.
    //--------------------------------------------------------------------
    always @(posedge clk) begin
        if (past_valid) begin
            cover (state_probe == 4'd0);  // ST_CLEAR reached (trivially, but confirms it's wired up)
        end
    end

endmodule
