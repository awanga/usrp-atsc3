// cordic_formal.v — Formal harness top for hdl/rtl/common/cordic.v
//
// Verification-only. Not synthesized, not part of rtl/. Instantiates the
// flatten+expose-generated cordic_bare (produced earlier in cordic.sby's
// [script] -- see that file and cordic_bare.v's header for why this
// two-stage flow replaces the `bind`-based pattern used elsewhere in this
// repo's formal work: `bind` and plain hierarchical dotted references both
// silently fail to connect in this Yosys build, so the exposed ports here
// are the only verified-working way to reach cordic's internal
// state/iter/mode_r/special_zero_r from a formal top module.
//
// Generates free (solver-chosen) stimulus on every input each cycle and
// asserts both black-box (port-only) and white-box (exposed-internal-state)
// protocol properties. Formal here targets control-flow/protocol safety
// only (FSM legality, the iteration-count bound, the start/busy/done
// handshake) -- numerical correctness (does this actually compute
// cos/sin/atan2/magnitude right) is cocotb's job (test_cordic.py), not this
// proof's.
//
// Run: cd hdl/formal && sby -f cordic.sby

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
