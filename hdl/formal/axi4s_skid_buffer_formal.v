// axi4s_skid_buffer_formal.v — Formal harness for axi4s_skid_buffer
//
// Main property (data integrity vs a ghost model): beats leave in the
// order they were accepted, each exactly once, with TDATA/TLAST
// unchanged. Ghost counters number accepted input beats and fired output
// beats; the solver picks an arbitrary index K (anyconst), the ghost
// records beat K's data when it is accepted, and output beat K must carry
// it. The occupancy invariant (accepted - fired == m_axis_tvalid +
// skid_valid, never more than 2) and the position of beat K inside the
// two storage slots are asserted too, which is what makes k-induction
// close. Supplementary: AXI4-S output stability, registered s_axis_tready,
// reset clears both slots, skid capture/drain.
//
// Assumptions: only that the first cycle is a reset. Every input is
// otherwise free each cycle -- the module must not lose or duplicate an
// accepted beat even if the producer drops TVALID without a handshake.
//
// DATA_WIDTH is shrunk to 4; the module does no arithmetic on TDATA.
// Depth 20 is far past the deepest event (both slots full, then drained:
// 4 cycles). The flatten+expose flow reaches skid_valid/skid_tdata/
// skid_tlast; see hdl/docs/formal_conventions.md.
//
// Run: hdl/formal/run_formal.sh axi4s_skid_buffer

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

    // Probes wired to the DUT's internal skid registers via the ports
    // axi4s_skid_buffer.sby's [script] exposed on
    // axi4s_skid_buffer_bare -- see that file's header.
    wire [DATA_WIDTH-1:0] skid_tdata_probe;
    wire                  skid_tlast_probe;
    wire                  skid_valid_probe;

    // No #(.DATA_WIDTH(...)) override here: stage 1's flatten pass (see
    // axi4s_skid_buffer.sby) specializes axi4s_skid_buffer_bare for the
    // width it was elaborated with and drops the parameter entirely --
    // the generated module's ports are already fixed at that width.
    axi4s_skid_buffer_bare dut_top (
        .clk(clk),
        .rst(rst),
        .s_axis_tdata(s_axis_tdata),
        .s_axis_tvalid(s_axis_tvalid),
        .s_axis_tready(s_axis_tready),
        .s_axis_tlast(s_axis_tlast),
        .m_axis_tdata(m_axis_tdata),
        .m_axis_tvalid(m_axis_tvalid),
        .m_axis_tready(m_axis_tready),
        .m_axis_tlast(m_axis_tlast),
        .\dut.skid_tdata (skid_tdata_probe),
        .\dut.skid_tlast (skid_tlast_probe),
        .\dut.skid_valid (skid_valid_probe)
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
    // reset-clears-state property below covers that case).
    always @(*) begin
        if (!past_valid) begin
            assume (rst);
        end
    end

    reg prev_rst;
    reg prev_s_valid, prev_s_ready, prev_s_tlast;
    reg [DATA_WIDTH-1:0] prev_s_tdata;
    reg prev_m_valid, prev_m_ready;
    reg [DATA_WIDTH-1:0] prev_m_tdata;
    reg prev_m_tlast;
    reg prev_skid_valid;

    always @(posedge clk) begin
        prev_rst        <= rst;
        prev_s_valid    <= s_axis_tvalid;
        prev_s_ready    <= s_axis_tready;
        prev_s_tdata    <= s_axis_tdata;
        prev_s_tlast    <= s_axis_tlast;
        prev_m_valid    <= m_axis_tvalid;
        prev_m_ready    <= m_axis_tready;
        prev_m_tdata    <= m_axis_tdata;
        prev_m_tlast    <= m_axis_tlast;
        prev_skid_valid <= skid_valid_probe;
    end

    //--------------------------------------------------------------------
    // Ghost model: data integrity and ordering
    //--------------------------------------------------------------------
    (* anyconst *) reg [3:0] ghost_k;
    reg  [3:0]            in_cnt, out_cnt;
    reg                   ghost_armed;
    reg  [DATA_WIDTH-1:0] ghost_data;
    reg                   ghost_last;
    wire                  in_fire  = s_axis_tvalid && s_axis_tready;
    wire                  out_fire = m_axis_tvalid && m_axis_tready;
    wire [3:0]            occupancy = in_cnt - out_cnt;
    wire [3:0]            k_pos     = ghost_k - out_cnt;  // 0 = output register, 1 = skid

    always @(posedge clk) begin
        if (rst) begin
            in_cnt      <= 4'd0;
            out_cnt     <= 4'd0;
            ghost_armed <= 1'b0;
        end else begin
            if (in_fire) in_cnt <= in_cnt + 4'd1;
            if (out_fire) out_cnt <= out_cnt + 4'd1;
            if (in_fire && in_cnt == ghost_k) begin
                ghost_armed <= 1'b1;
                ghost_data  <= s_axis_tdata;
                ghost_last  <= s_axis_tlast;
            end else if (out_fire && out_cnt == ghost_k) begin
                ghost_armed <= 1'b0;
            end
        end
    end

    always @(*) begin
        if (past_valid && !rst) begin
            assert (occupancy == {3'd0, m_axis_tvalid} + {3'd0, skid_valid_probe});
            assert (!skid_valid_probe || m_axis_tvalid);  // skid only behind a full output
            assert (ghost_armed == (k_pos < occupancy));
            if (ghost_armed && k_pos == 4'd0) begin
                assert (m_axis_tdata == ghost_data);
                assert (m_axis_tlast == ghost_last);
            end
            if (ghost_armed && k_pos == 4'd1) begin
                assert (skid_tdata_probe == ghost_data);
                assert (skid_tlast_probe == ghost_last);
            end
        end
    end

    //--------------------------------------------------------------------
    // Black-box property: AXI4-S output stability. Once TVALID is high
    // and not accepted, it must stay high next cycle with TDATA/TLAST
    // unchanged. Uses only DUT ports, no exposed internal state.
    //--------------------------------------------------------------------
    always @(posedge clk) begin
        if (past_valid && !prev_rst && prev_m_valid && !prev_m_ready) begin
            assert (m_axis_tvalid);
            assert (m_axis_tdata == prev_m_tdata);
            assert (m_axis_tlast == prev_m_tlast);
        end
    end

    //--------------------------------------------------------------------
    // White-box properties, via the exposed probes above.
    //--------------------------------------------------------------------

    // s_axis_tready is purely registered: a direct restatement of the
    // assign, but cheap and catches any future refactor that sneaks in a
    // combinational dependency on m_axis_tready.
    always @(posedge clk) begin
        if (past_valid) begin
            assert (s_axis_tready == !skid_valid_probe);
        end
    end

    // Reset synchronously clears both storage slots.
    always @(posedge clk) begin
        if (past_valid && prev_rst) begin
            assert (!m_axis_tvalid);
            assert (!skid_valid_probe);
        end
    end

    // No-drop: a beat accepted while the output register is full and
    // stalled must land intact in the skid register on the next cycle.
    always @(posedge clk) begin
        if (past_valid && !prev_rst && prev_s_valid && prev_s_ready &&
            prev_m_valid && !prev_m_ready) begin
            assert (skid_valid_probe);
            assert (skid_tdata_probe == prev_s_tdata);
            assert (skid_tlast_probe == prev_s_tlast);
        end
    end

    // Skid drains to the output register as soon as it's free.
    always @(posedge clk) begin
        if (past_valid && !prev_rst && prev_skid_valid &&
            (!prev_m_valid || prev_m_ready)) begin
            assert (m_axis_tvalid);
        end
    end

    // Liveness sanity: both storage slots can be occupied simultaneously
    // (proves the skid path is actually reachable, not vacuously unused).
    always @(posedge clk) begin
        if (past_valid) begin
            cover (m_axis_tvalid && skid_valid_probe);
            // Beat K delivered from the skid path after a stall.
            cover (ghost_armed && out_fire && out_cnt == ghost_k && prev_skid_valid);
        end
    end

endmodule
