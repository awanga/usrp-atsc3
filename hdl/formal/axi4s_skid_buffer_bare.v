// axi4s_skid_buffer_bare.v — Trivial single-instance wrapper, used only to
// seed the flatten+expose step in axi4s_skid_buffer.sby's [script] that
// promotes axi4s_skid_buffer's internal skid_valid/skid_tdata/skid_tlast
// registers to top-level ports so the formal harness can reference them.
//
// Replaces the earlier `bind`-based white-box checker
// (axi4s_skid_buffer_checks.sv/_bind.sv): this Yosys build's plain
// (non-Verific) frontend accepts `bind` syntactically but never actually
// instantiates the bound module -- confirmed by injecting a deliberately-
// false assert into a `bind`-attached checker and finding it still proved,
// i.e. the whole white-box side of this proof had been silently vacuous.
// A plain hierarchical dotted reference (`dut.skid_valid` written directly
// in an always block outside the DUT) has the same failure mode: Yosys
// silently treats the unresolved name as a fresh, disconnected free
// variable instead of a real connection or an error. `expose` (a
// first-class Yosys pass, not a Verilog-source construct) is the mechanism
// verified to actually work -- see axi4s_skid_buffer.sby's [script] for
// the full two-stage flow and hdl/docs/toolchain.md for the verification
// method (same fix applied to hdl/formal/cordic_bare.v/cordic.sby).
//
// Verification-only. Not synthesized, not part of rtl/.

module axi4s_skid_buffer_bare #(
    parameter DATA_WIDTH = 4  // small, per the small-parameterization
                              // convention: proofs run at a width narrow
                              // enough for BMC/induction to be fast and
                              // exhaustive over TDATA, not to model any
                              // real block's actual width.
) (
    input  wire clk,
    input  wire rst,

    input  wire [DATA_WIDTH-1:0] s_axis_tdata,
    input  wire                  s_axis_tvalid,
    output wire                  s_axis_tready,
    input  wire                  s_axis_tlast,

    output wire [DATA_WIDTH-1:0] m_axis_tdata,
    output wire                  m_axis_tvalid,
    input  wire                  m_axis_tready,
    output wire                  m_axis_tlast
);

    axi4s_skid_buffer #(
        .DATA_WIDTH(DATA_WIDTH)
    ) dut (
        .clk(clk),
        .rst(rst),
        .s_axis_tdata(s_axis_tdata),
        .s_axis_tvalid(s_axis_tvalid),
        .s_axis_tready(s_axis_tready),
        .s_axis_tlast(s_axis_tlast),
        .m_axis_tdata(m_axis_tdata),
        .m_axis_tvalid(m_axis_tvalid),
        .m_axis_tready(m_axis_tready),
        .m_axis_tlast(m_axis_tlast)
    );

endmodule
