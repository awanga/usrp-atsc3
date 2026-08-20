// axi4s_skid_buffer.v — AXI4-Stream register slice ("skid buffer")
//
// AXI4-S: TDATA=<DATA_WIDTH bits, opaque> TVALID TREADY TLAST
//
// Breaks the combinational path from m_axis_tready back to s_axis_tready:
// s_axis_tready depends only on registered state (skid_valid), never on
// m_axis_tready directly. Full throughput (one transfer/cycle) is preserved;
// when the downstream stalls, one extra beat is captured in the skid
// register so upstream doesn't have to combinationally see the stall.
//
// This is the shared pipeline-register primitive instantiated between RTL
// blocks per hdl/docs/ (Phase 9.0 common infra). IEEE 1364-2001 Verilog.

`include "axi4s_types.vh"

module axi4s_skid_buffer #(
    parameter DATA_WIDTH = `ATSC3_SAMPLE_WIDTH
) (
    input  wire clk,
    input  wire rst,  // synchronous, active-high

    `AXI4S_SLAVE(s_axis, DATA_WIDTH),
    `AXI4S_MASTER(m_axis, DATA_WIDTH)
);

    reg [DATA_WIDTH-1:0] skid_tdata;
    reg                  skid_tlast;
    reg                  skid_valid;

    // Purely registered: no combinational dependency on m_axis_tready.
    assign s_axis_tready = !skid_valid;

    always @(posedge clk) begin
        if (rst) begin
            m_axis_tdata  <= {DATA_WIDTH{1'b0}};
            m_axis_tvalid <= 1'b0;
            m_axis_tlast  <= 1'b0;
            skid_tdata    <= {DATA_WIDTH{1'b0}};
            skid_tlast    <= 1'b0;
            skid_valid    <= 1'b0;
        end else if (skid_valid) begin
            // Skid is occupied: s_axis_tready is low, so no new input this
            // cycle. Drain skid into the output register as soon as it's
            // free (empty or firing this cycle).
            if (!m_axis_tvalid || m_axis_tready) begin
                m_axis_tdata  <= skid_tdata;
                m_axis_tlast  <= skid_tlast;
                m_axis_tvalid <= 1'b1;
                skid_valid    <= 1'b0;
            end
        end else if (s_axis_tvalid) begin
            // s_axis_tready is high here (skid empty), so this is a transfer.
            if (!m_axis_tvalid || m_axis_tready) begin
                // Output register free: bypass straight through.
                m_axis_tdata  <= s_axis_tdata;
                m_axis_tlast  <= s_axis_tlast;
                m_axis_tvalid <= 1'b1;
            end else begin
                // Output register stalled: capture into skid instead of
                // dropping the accepted beat.
                skid_tdata <= s_axis_tdata;
                skid_tlast <= s_axis_tlast;
                skid_valid <= 1'b1;
            end
        end else if (m_axis_tvalid && m_axis_tready) begin
            // No new input; output fired and nothing to replace it with.
            m_axis_tvalid <= 1'b0;
        end
    end

endmodule
