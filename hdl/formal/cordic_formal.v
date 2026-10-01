// cordic_formal.v — Formal harness for hdl/rtl/common/cordic.v
//
// Main property (handshake and data integrity vs a ghost model): the
// mode and the degenerate-vector condition are captured on the edge that
// accepts start; done must arrive exactly CORDIC_LATENCY (16) cycles
// later, or 2 for vector (0, 0); busy is high until then; the captured
// mode is the one computed; out_a/out_b change only on done; and vector
// (0, 0) yields exactly (0, 0) (atan2(0, 0) = 0 by convention, magnitude
// 0). The FSM state and iteration index are pinned to the ghost's elapsed
// cycle count, which closes k-induction.
//
// Not proved here: the angle/magnitude/cos/sin values themselves. There
// is no tractable independent formal oracle for atan2 or a rotation; they
// are checked bit-exact against lib/dsp/cordic.cc in test_cordic.py.
//
// Assumptions: only that the first cycle is a reset. start may be raised
// while busy (the core must ignore it) and inputs change every cycle.
// Depth 40 exceeds the deepest event (a full 16-cycle computation
// followed by a back-to-back one). White-box probes come from the
// flatten+expose flow; see hdl/docs/formal_conventions.md.
//
// Run: hdl/formal/run_formal.sh cordic

`include "cordic_types.vh"

module cordic_formal (
    input wire clk,
    input wire rst
);

    localparam ST_IDLE   = 2'd0,
               ST_PREP   = 2'd1,
               ST_ITER   = 2'd2,
               ST_FINISH = 2'd3;

    reg                start;
    reg                mode;
    reg  signed [15:0] in_a;
    reg  signed [15:0] in_b;

    wire               busy;
    wire               done;
    wire signed [15:0] out_a;
    wire signed [31:0] out_b;

    // Probes wired to cordic's internal registers via the ports
    // cordic.sby's [script] exposed on cordic_bare -- not DUT ports in the
    // ordinary sense, but the only ones this harness needs beyond
    // start/mode/in_a/in_b/busy/done/out_a/out_b for its white-box checks.
    wire [1:0] state_probe;
    wire [3:0] iter_probe;
    wire       mode_r_probe;
    wire       special_zero_r_probe;

    cordic_bare dut_top (
        .clk   (clk),
        .rst   (rst),
        .start (start),
        .mode  (mode),
        .in_a  (in_a),
        .in_b  (in_b),
        .busy  (busy),
        .done  (done),
        .out_a (out_a),
        .out_b (out_b),
        .\dut.state           (state_probe),
        .\dut.iter            (iter_probe),
        .\dut.mode_r          (mode_r_probe),
        .\dut.special_zero_r  (special_zero_r_probe)
    );

    // start/mode/in_a/in_b are `reg` with no always-block driver: free
    // primary inputs the solver re-picks every cycle (the standard
    // SymbiYosys stimulus idiom), not literal don't-care regs. No
    // assumption is placed on start only pulsing while !busy: the DUT
    // ignores start outside ST_IDLE by construction (see cordic.v), so a
    // spurious start while busy is a real, valid case to prove safe rather
    // than something to assume away.

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;

    // Force a reset on the very first cycle so the design starts from a
    // known state; free thereafter.
    always @(*) begin
        if (!past_valid) begin
            assume (rst);
        end
    end

    reg       prev_rst;
    reg [1:0] prev_state_probe;
    reg       prev_done;

    always @(posedge clk) begin
        prev_rst          <= rst;
        prev_state_probe  <= state_probe;
        prev_done         <= done;
    end

    //--------------------------------------------------------------------
    // Black-box property: busy and done are mutually exclusive. Uses only
    // DUT ports, no exposed internal state.
    //--------------------------------------------------------------------
    always @(posedge clk) begin
        if (past_valid) begin
            assert (!(busy && done));
        end
    end

    //--------------------------------------------------------------------
    // White-box properties, via the exposed probes above.
    //--------------------------------------------------------------------

    // FSM only ever occupies one of the four defined states. Structurally
    // guaranteed by the 2-bit encoding plus a `default: state <= ST_IDLE`
    // catch-all in cordic.v, but asserted directly rather than trusted.
    always @(posedge clk) begin
        if (past_valid) begin
            assert (state_probe == ST_IDLE || state_probe == ST_PREP ||
                    state_probe == ST_ITER || state_probe == ST_FINISH);
        end
    end

    // Iteration-count bound: the shared iterative datapath is reused for
    // exactly `CORDIC_ITERATIONS` steps and must never run past the last
    // valid atan-table index.
    always @(posedge clk) begin
        if (past_valid) begin
            assert (iter_probe <= `CORDIC_ITERATIONS - 1);
        end
    end

    // busy tracks "not idle" exactly: set the same cycle state leaves
    // ST_IDLE, held through ST_PREP/ST_ITER/ST_FINISH, cleared the same
    // cycle state returns to ST_IDLE.
    always @(posedge clk) begin
        if (past_valid) begin
            assert (busy == (state_probe != ST_IDLE));
        end
    end

    // done is a strict one-cycle pulse: never high two cycles running.
    always @(posedge clk) begin
        if (past_valid && prev_done) begin
            assert (!done);
        end
    end

    //--------------------------------------------------------------------
    // Ghost model of the computation in flight
    //--------------------------------------------------------------------
    localparam [4:0] LATENCY      = `CORDIC_ITERATIONS + 2;
    localparam [4:0] ZERO_LATENCY = 5'd2;

    wire        accepted = start && !busy;
    reg         g_active, g_mode, g_zero;
    reg  [4:0]  g_age;       // edges since the accepting edge
    reg  [15:0] prev_out_a;
    reg  [31:0] prev_out_b;
    wire [4:0]  g_latency = g_zero ? ZERO_LATENCY : LATENCY;

    always @(posedge clk) begin
        prev_out_a <= out_a;
        prev_out_b <= out_b;
        if (rst) begin
            g_active <= 1'b0;
            g_age    <= 5'd0;
        end else if (accepted) begin
            g_active <= 1'b1;
            g_mode   <= mode;
            g_zero   <= (mode == `CORDIC_MODE_VECTOR) && in_a == 16'sd0 && in_b == 16'sd0;
            g_age    <= 5'd0;
        end else if (g_active) begin
            if (done) g_active <= 1'b0;
            else g_age <= g_age + 5'd1;
        end
    end

    always @(*) begin
        if (past_valid && !rst) begin
            if (done) begin
                assert (g_active && g_age == g_latency);
                if (g_zero) assert (out_a == 16'sd0 && out_b == 32'sd0);
            end
            if (g_active && g_age < g_latency) assert (busy && !done);
            if (!g_active) assert (!busy && !done);
            if (busy) assert (mode_r_probe == g_mode && special_zero_r_probe == g_zero);
            // FSM position as a function of elapsed cycles.
            if (busy && g_age == 5'd0) assert (state_probe == ST_PREP);
            if (busy && !g_zero && g_age >= 5'd1 && g_age <= `CORDIC_ITERATIONS)
                assert (state_probe == ST_ITER && iter_probe == g_age - 5'd1);
            if (busy && g_age == g_latency - 5'd1) assert (state_probe == ST_FINISH);
        end
        if (past_valid && !prev_rst && !rst && !done) begin
            assert (out_a == prev_out_a && out_b == prev_out_b);
        end
    end

    // done only ever follows a cycle where state was ST_FINISH (the only
    // branch that sets it).
    always @(posedge clk) begin
        if (past_valid && done) begin
            assert (prev_state_probe == ST_FINISH);
        end
    end

    // Reachability: the iteration loop actually runs to completion (not
    // vacuously short-circuited every time), for both shared-datapath
    // modes, and the degenerate vector(0,0) bypass is actually reachable
    // too (skips ST_ITER entirely -- worth confirming that path exists
    // rather than being unreachable dead code).
    always @(posedge clk) begin
        if (past_valid) begin
            cover (state_probe == ST_ITER && iter_probe == `CORDIC_ITERATIONS - 1);
            cover (done && mode_r_probe == `CORDIC_MODE_ROTATE && !special_zero_r_probe);
            cover (done && mode_r_probe == `CORDIC_MODE_VECTOR && !special_zero_r_probe);
            cover (done && mode_r_probe == `CORDIC_MODE_VECTOR && special_zero_r_probe);
        end
    end

endmodule
