// cordic_bare.v — Trivial single-instance wrapper, used only to seed the
// flatten+expose step in cordic.sby's [script] that promotes cordic's
// internal state/iter/mode_r/special_zero_r registers to top-level ports
// so the formal harness can reference them.
//
// Why this exists instead of the `bind`-based white-box pattern used
// elsewhere in this repo's earlier formal work (see
// hdl/docs/formal_conventions.md): this Yosys build's plain (non-Verific)
// frontend accepts `bind` syntactically but never actually instantiates
// the bound module -- confirmed by injecting a deliberately-false assert
// into a `bind`-attached checker and finding it still proves. The same is
// true of a plain hierarchical dotted reference (e.g. `dut.state` written
// directly in an always block outside the DUT): Yosys silently treats the
// unresolved name as a fresh, disconnected free variable rather than a
// real connection or an error. `expose` (a first-class Yosys pass, not a
// Verilog-source construct) is the mechanism that's actually verified to
// work in this toolchain -- see cordic.sby's [script] for the full
// two-stage flow and hdl/docs/toolchain.md for the verification method.
//
// Verification-only. Not synthesized, not part of rtl/.

module cordic_bare (
    input  wire                clk,
    input  wire                rst,
    input  wire                start,
    input  wire                mode,
    input  wire signed [15:0]  in_a,
    input  wire signed [15:0]  in_b,
    output wire                busy,
    output wire                done,
    output wire signed [15:0]  out_a,
    output wire signed [31:0]  out_b
);

    cordic dut (
        .clk   (clk),
        .rst   (rst),
        .start (start),
        .mode  (mode),
        .in_a  (in_a),
        .in_b  (in_b),
        .busy  (busy),
        .done  (done),
        .out_a (out_a),
        .out_b (out_b)
    );

endmodule
