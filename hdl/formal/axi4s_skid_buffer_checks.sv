// axi4s_skid_buffer_checks.sv — White-box formal properties for
// axi4s_skid_buffer, attached via `bind` (see axi4s_skid_buffer.sby).
//
// Verification-only, SystemVerilog. Never included by rtl/ or synthesized:
// bind lives entirely in the formal harness, so axi4s_skid_buffer.v itself
// stays plain IEEE 1364-2001 with no formal-specific content. Because a
// bound instance is elaborated as if declared inside the target module, it
// sees skid_valid/skid_tdata/skid_tlast and all ports by their bare names
// -- no hierarchical dotted references needed (those silently create
// disconnected implicit wires in Yosys rather than a real connection, which
// is what the first version of this harness got wrong).

module axi4s_skid_buffer_checks;

    // Matches axi4s_skid_buffer_formal's DATA_WIDTH. Yosys resolves a
    // bound instance's own declarations before attaching it to the target
    // scope, so this can't be inferred from the target's DATA_WIDTH
    // parameter at elaboration time -- kept as a local constant instead,
    // which is fine since this checker is only ever bound within this one
    // proof (fixed small width, per the small-parameterization convention).
    localparam CHK_DATA_WIDTH = 4;

    reg past_valid_c;
    initial past_valid_c = 1'b0;
    always @(posedge clk) past_valid_c <= 1'b1;

    reg prev_rst_c;
    reg prev_s_valid_c, prev_s_ready_c, prev_s_tlast_c;
    reg [CHK_DATA_WIDTH-1:0] prev_s_tdata_c;
    reg prev_m_valid_c, prev_m_ready_c;
    reg prev_skid_valid_c;

    always @(posedge clk) begin
        prev_rst_c        <= rst;
        prev_s_valid_c     <= s_axis_tvalid;
        prev_s_ready_c     <= s_axis_tready;
        prev_s_tdata_c     <= s_axis_tdata;
        prev_s_tlast_c     <= s_axis_tlast;
        prev_m_valid_c     <= m_axis_tvalid;
        prev_m_ready_c     <= m_axis_tready;
        prev_skid_valid_c  <= skid_valid;
    end

    // s_axis_tready is purely registered: a direct restatement of the
    // assign, but cheap and catches any future refactor that sneaks in a
    // combinational dependency on m_axis_tready.
    always @(posedge clk) begin
        if (past_valid_c) begin
            assert (s_axis_tready == !skid_valid);
        end
    end

    // Reset synchronously clears both storage slots.
    always @(posedge clk) begin
        if (past_valid_c && prev_rst_c) begin
            assert (!m_axis_tvalid);
            assert (!skid_valid);
        end
    end

    // No-drop: a beat accepted while the output register is full and
    // stalled must land intact in the skid register on the next cycle.
    always @(posedge clk) begin
        if (past_valid_c && !prev_rst_c && prev_s_valid_c && prev_s_ready_c &&
            prev_m_valid_c && !prev_m_ready_c) begin
            assert (skid_valid);
            assert (skid_tdata == prev_s_tdata_c);
            assert (skid_tlast == prev_s_tlast_c);
        end
    end

    // Skid drains to the output register as soon as it's free.
    always @(posedge clk) begin
        if (past_valid_c && !prev_rst_c && prev_skid_valid_c &&
            (!prev_m_valid_c || prev_m_ready_c)) begin
            assert (m_axis_tvalid);
        end
    end

    // Liveness sanity: both storage slots can be occupied simultaneously
    // (proves the skid path is actually reachable, not vacuously unused).
    always @(posedge clk) begin
        if (past_valid_c) begin
            cover (m_axis_tvalid && skid_valid);
        end
    end

endmodule
