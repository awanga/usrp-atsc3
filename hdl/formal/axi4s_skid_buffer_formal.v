// axi4s_skid_buffer_formal.v — Formal harness top for axi4s_skid_buffer
//
// Verification-only. Not synthesized, not part of rtl/. Generates free
// (solver-chosen) stimulus on every input each cycle and asserts black-box
// (port-only) protocol properties. White-box properties that need the
// DUT's internal skid_valid/skid_tdata/skid_tlast state live in
// axi4s_skid_buffer_checks.sv, attached via `bind` (see the .sby script) --
// that keeps this file's DUT instantiation a plain, ordinary one, and keeps
// axi4s_skid_buffer.v itself free of any formal-specific content.
//
// Run: cd hdl/formal && sby -f axi4s_skid_buffer.sby

`include "axi4s_types.vh"

module axi4s_skid_buffer_formal #(
    parameter DATA_WIDTH = 4  // small, per the small-parameterization
                              // convention: proofs run at a width narrow
                              // enough for BMC/induction to be fast and
                              // exhaustive over TDATA, not to model any
                              // real block's actual width.
) (
    input wire clk,
    input wire rst
);

    reg  [DATA_WIDTH-1:0] s_axis_tdata;
    reg                   s_axis_tvalid;
    wire                  s_axis_tready;
    reg                   s_axis_tlast;

    wire [DATA_WIDTH-1:0] m_axis_tdata;
    wire                  m_axis_tvalid;
    reg                   m_axis_tready;
    wire                  m_axis_tlast;

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

    // s_axis_tdata/tvalid/tlast/m_axis_tready are `reg` with no always-block
    // driver: that makes them free primary inputs the solver re-picks every
    // cycle (the standard SymbiYosys stimulus idiom), not literal don't-care
    // regs.

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;

    // Force a reset on the very first cycle so the design starts from a
    // known state; free thereafter (rst can also pulse later -- the
    // reset-clears-state property in the bound checker covers that case).
    always @(*) begin
        if (!past_valid) begin
            assume (rst);
        end
    end

    //--------------------------------------------------------------------
    // Black-box property: AXI4-S output stability. Once TVALID is high
    // and not accepted, it must stay high next cycle with TDATA/TLAST
    // unchanged. Uses only DUT ports, no internal state.
    //--------------------------------------------------------------------

    reg prev_m_valid;
    reg prev_m_ready;
    reg [DATA_WIDTH-1:0] prev_m_tdata;
    reg prev_m_tlast;
    reg prev_rst;

    always @(posedge clk) begin
        prev_m_valid <= m_axis_tvalid;
        prev_m_ready <= m_axis_tready;
        prev_m_tdata <= m_axis_tdata;
        prev_m_tlast <= m_axis_tlast;
        prev_rst     <= rst;
    end

    always @(posedge clk) begin
        if (past_valid && !prev_rst && prev_m_valid && !prev_m_ready) begin
            assert (m_axis_tvalid);
            assert (m_axis_tdata == prev_m_tdata);
            assert (m_axis_tlast == prev_m_tlast);
        end
    end

endmodule
