// timing_recovery_formal.v — Formal harness for timing_recovery
//
// Main property (data integrity vs a ghost model, input path): every
// accepted input beat is written exactly once, unmodified, to ring-buffer
// slot (accepted count mod 128) -- the slot the interpolator later
// addresses relative to -- and the reset-time clear sweep writes only
// zeros. The interpolated output values themselves (polyphase FIR,
// Gardner TED, loop filter) are not proved: there is no tractable
// independent formal oracle for them, so they are checked bit-exact
// against lib/sync/timing_recovery.cc in test_timing_recovery.py and
// test_polyphase_fir.py.
//
// Supplementary:
//   - FSM legality.
//   - mu_q16's reset value equals (cfg_initial_offset_q15 reinterpreted
//     as unsigned) * 2 mod 65536, computed here independently of the
//     RTL's bit-slice -- the derivation timing_recovery.v's header gives
//     for matching the C++'s wrap of the initial offset.
//   - s_axis_tready / m_axis_tvalid pinned to their controlling FSM
//     states (ST_IDLE / ST_OUTPUT); the purely temporal forms are true
//     but not inductive on their own.
//   - AXI4-S output stability while stalled; reset clears the handshake
//     outputs.
//
// Assumptions, each encoding a caller contract:
//   - AXI4-S producer rule: an offered beat is held until accepted (only
//     the output-stability check depends on it).
// Configuration inputs are otherwise free every cycle: they are latched
// at reset (cfg_locked is read live by design).
//
// Multipliers are cut (cutpoint), sound because no property depends on a
// product's value. Depth 20 covers the input path (accept -> write is two
// cycles), but the first emitted symbol needs the 128-cycle clear sweep
// plus 32 samples, past practical BMC depth, so output-side events are
// not covered here; non-vacuity of the proved properties comes from the
// committed mutants in hdl/mutants. White-box probes come from the
// flatten+expose flow; see hdl/docs/formal_conventions.md.
//
// Run: hdl/formal/run_formal.sh timing_recovery

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
    wire        buf_wr_en_probe;
    wire [6:0]  buf_wr_addr_probe;
    wire [31:0] buf_wr_data_probe;
    wire [6:0]  buf_write_idx_probe;
    wire [15:0] x_re_probe, x_im_probe;

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
        .\dut.state         (state_probe),
        .\dut.mu_q16        (mu_q16_probe),
        .\dut.buf_wr_en     (buf_wr_en_probe),
        .\dut.buf_wr_addr   (buf_wr_addr_probe),
        .\dut.buf_wr_data   (buf_wr_data_probe),
        .\dut.buf_write_idx (buf_write_idx_probe),
        .\dut.x_re          (x_re_probe),
        .\dut.x_im          (x_im_probe)
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
    // Ghost model: input path data integrity. Every accepted beat is
    // written once, unmodified, to ring-buffer slot (accepted count mod
    // 128) -- the slot the interpolator later addresses relative to.
    //--------------------------------------------------------------------
    localparam [3:0] ST_CLEAR = 4'd0, ST_IDLE = 4'd1, ST_WRITE = 4'd2, ST_CHECK = 4'd3;

    wire       in_fire     = s_axis_tvalid && s_axis_tready;
    wire       sample_wr   = buf_wr_en_probe && state_probe == ST_CHECK;
    reg        g_pending;
    reg [31:0] g_data;
    reg [6:0]  g_widx;

    always @(posedge clk) begin
        if (rst) begin
            g_pending <= 1'b0;
            g_widx    <= 7'd0;
        end else begin
            if (in_fire) begin
                g_pending <= 1'b1;
                g_data    <= s_axis_tdata;
            end else if (sample_wr) begin
                g_pending <= 1'b0;
                g_widx    <= g_widx + 7'd1;
            end
        end
    end

    always @(*) begin
        if (past_valid && !rst) begin
            if (sample_wr) begin
                assert (g_pending);
                assert (buf_wr_addr_probe == g_widx);
                assert (buf_wr_data_probe == g_data);
            end
            if (buf_wr_en_probe && !sample_wr) assert (buf_wr_data_probe == 32'd0);  // clear sweep
            assert (g_pending == (state_probe == ST_WRITE || sample_wr));
            if (state_probe == ST_WRITE) assert ({x_re_probe, x_im_probe} == g_data);
            assert (buf_write_idx_probe == (sample_wr ? g_widx + 7'd1 : g_widx));
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
