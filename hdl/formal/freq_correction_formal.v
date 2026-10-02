// freq_correction_formal.v — Formal harness for freq_correction
//
// Main property (data integrity vs a ghost model of the NCO phase): a
// ghost accumulator follows lib/sync/freq_correction.cc's semantics --
// advance by the increment after each sample, cleared by reset_phase()
// before a sample accepted on the same edge -- and the cycle after each
// accepted sample the DUT has started a CORDIC rotation of exactly the
// ghost's phase (top 16 bits) and latched that sample's TLAST; its own
// accumulator equals the ghost's at all times. One output beat per input
// beat: accepted-minus-emitted (ghost counters mod 256) always equals
// the samples in flight plus the pending output, and a newly loaded
// output carries the sample's TLAST. Supplementary: the output bound
// (never -32768 on either rail, for any product), TREADY only when
// idle, and AXI4-S output stability.
//
// Cut: the four products (freq_correction.sby). Output values are
// checked bit-exact by test_freq_correction.py against the C++; the
// CORDIC has its own harness (cordic.sby).
//
// Assumptions: only that the first cycle is a reset. cfg_phase_inc and
// phase_reset are free every cycle (the RTL samples them per beat).
//
// Bounds: depth 44. Cover reaches a loaded output (accept, 16 CORDIC
// cycles, products, load: about 20 cycles); the deepest event the
// properties guard, a second sample finishing while the first output is
// still stalled, needs about 40.
//
// Run: hdl/formal/run_formal.sh freq_correction

`include "axi4s_types.vh"

module freq_correction_formal (
    input wire clk,
    input wire rst
);

    localparam ST_IDLE = 2'd0, ST_OUT = 2'd2;

    reg  [31:0] cfg_phase_inc;
    reg         phase_reset;

    reg  [31:0] s_axis_tdata;
    reg         s_axis_tvalid;
    wire        s_axis_tready;
    reg         s_axis_tlast;

    wire [31:0] m_axis_tdata;
    wire        m_axis_tvalid;
    reg         m_axis_tready;
    wire        m_axis_tlast;

    wire [1:0]  state_probe;
    wire [31:0] acc_probe;
    wire        cordic_start_probe;
    wire [15:0] cordic_angle_probe;
    wire        in_last_probe;

    freq_correction_bare dut_top (
        .clk           (clk),
        .rst           (rst),
        .cfg_phase_inc (cfg_phase_inc),
        .phase_reset   (phase_reset),
        .s_axis_tdata  (s_axis_tdata),
        .s_axis_tvalid (s_axis_tvalid),
        .s_axis_tready (s_axis_tready),
        .s_axis_tlast  (s_axis_tlast),
        .m_axis_tdata  (m_axis_tdata),
        .m_axis_tvalid (m_axis_tvalid),
        .m_axis_tready (m_axis_tready),
        .m_axis_tlast  (m_axis_tlast),
        .\dut.state        (state_probe),
        .\dut.acc          (acc_probe),
        .\dut.cordic_start (cordic_start_probe),
        .\dut.cordic_angle (cordic_angle_probe),
        .\dut.in_last      (in_last_probe)
    );

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;

    always @(*) begin
        if (!past_valid)
            assume (rst);
    end

    wire fire  = s_axis_tvalid && s_axis_tready;
    wire m_out = m_axis_tvalid && m_axis_tready;

    //--------------------------------------------------------------------
    // Ghost NCO phase and beat counters
    //--------------------------------------------------------------------

    reg  [31:0] g_acc;
    wire [31:0] g_phase = phase_reset ? 32'd0 : g_acc;  // reset_phase() first
    reg  [15:0] g_angle;
    reg         g_last;
    reg  [7:0]  g_in, g_out;

    always @(posedge clk) begin
        if (rst) begin
            g_acc <= 32'd0;
            g_in  <= 8'd0;
            g_out <= 8'd0;
        end else begin
            if (fire) begin
                g_acc   <= g_phase + cfg_phase_inc;
                g_angle <= g_phase[31:16];
                g_last  <= s_axis_tlast;
                g_in    <= g_in + 8'd1;
            end else if (phase_reset) begin
                g_acc <= 32'd0;
            end
            if (m_out)
                g_out <= g_out + 8'd1;
        end
    end

    reg        prev_rst, prev_fire, prev_load;
    reg        prev_m_valid, prev_m_ready, prev_m_last;
    reg [31:0] prev_m_data;
    always @(posedge clk) begin
        prev_rst     <= rst;
        prev_fire    <= fire && !rst;
        prev_load    <= !rst && state_probe == ST_OUT && (!m_axis_tvalid || m_axis_tready);
        prev_m_valid <= m_axis_tvalid;
        prev_m_ready <= m_axis_tready;
        prev_m_data  <= m_axis_tdata;
        prev_m_last  <= m_axis_tlast;
    end

    wire [7:0] in_flight = {7'd0, state_probe != ST_IDLE} + {7'd0, m_axis_tvalid};

    always @(*) begin
        if (past_valid && !rst && !prev_rst) begin
            // Main property
            assert (acc_probe == g_acc);
            if (prev_fire) begin
                assert (cordic_start_probe);
                assert (cordic_angle_probe == g_angle);
                assert (in_last_probe == g_last);
            end
            assert (g_in - g_out == in_flight);
            if (state_probe != ST_IDLE)
                assert (in_last_probe == g_last);
            if (prev_load) begin
                assert (m_axis_tvalid);
                assert (m_axis_tlast == g_last);
            end else if (prev_m_valid && !prev_m_ready) begin
                assert (m_axis_tvalid);
                assert (m_axis_tdata == prev_m_data);
                assert (m_axis_tlast == prev_m_last);
            end

            // Supplementary
            assert (state_probe <= ST_OUT);
            assert (s_axis_tready == (state_probe == ST_IDLE));
            if (m_axis_tvalid) begin
                assert (m_axis_tdata[31:16] != 16'h8000);
                assert (m_axis_tdata[15:0] != 16'h8000);
            end
        end
        if (past_valid && prev_rst)
            assert (!m_axis_tvalid && acc_probe == 32'd0);
    end

    always @(*) begin
        if (past_valid && !rst && !prev_rst) begin
            cover (prev_load && m_axis_tlast);
            cover (prev_fire && cordic_angle_probe != 16'd0);
        end
    end

endmodule
