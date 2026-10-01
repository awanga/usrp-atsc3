// bootstrap_detector_formal.v — Formal harness for
// hdl/rtl/sync/bootstrap_detector.v
//
// Main property (data integrity vs a ghost model): the detector's sample
// counter -- the source of every detection's reported sample index --
// always equals a ghost count of accepted input beats (compared mod 256,
// which keeps PDR fast and still catches any skipped or repeated count),
// and each accepted
// beat produces exactly one monitor update before the next beat is
// accepted. The correlator, metric and CFO values are not proved: there is
// no tractable independent formal oracle for them (and the multipliers
// are cut), so they are checked bit-exact against
// lib/sync/bootstrap_detector.cc in test_bootstrap_detector.py.
//
// Supplementary: FSM legality; correlator, window and history index
// bounds; the averaging window clamp (and stability while running); the
// P/R divides only above the energy floor (so never by zero); detect and
// re-arm states exclusive; AXI4-S input/output exclusivity, TLAST on the
// status word and output stability.
//
// Assumptions: only that the first cycle is a reset. Configuration is
// latched at reset; every input is otherwise free each cycle.
//
// Runs at L = 4 with a 4-entry window, multipliers cut, proved unbounded
// with ABC PDR (smtbmc/z3 cannot finish a depth-2 BMC here). PDR gives no
// cover, so non-vacuity comes from the committed mutants in hdl/mutants,
// checked with bounded BMC (prove_pdr.sh --mutant): a history-index wrap
// off-by-one (fails at frame 16), dividing below the energy floor (frame
// 9) and staying in detection after a detection (frame 162, two full
// samples in).
//
// Run: hdl/formal/run_formal.sh bootstrap_detector

`include "status_words.vh"

module bootstrap_detector_formal (
    input wire clk,
    input wire rst
);

    localparam HALF_SYMBOL = 4;
    localparam MAX_WIN     = 4;

    localparam [3:0] ST_IDLE         = 4'd1,
                     ST_DIV_START    = 4'd3,
                     ST_EMIT         = 4'd11;

    localparam signed [63:0] MIN_ENERGY = 64'sd65536;

    // Free stimulus (no driver: the solver picks a new value every cycle).
    reg [31:0]        cfg_sample_rate_hz;
    reg signed [15:0] cfg_threshold_q15;
    reg [31:0]        cfg_averaging_window;
    reg [31:0]        s_axis_tdata;
    reg               s_axis_tvalid;
    reg               s_axis_tlast;
    reg               m_axis_tready;

    wire                                  cfg_window_clamped;
    wire                                  s_axis_tready;
    wire [`BOOTSTRAP_DETECTION_WIDTH-1:0] m_axis_tdata;
    wire                                  m_axis_tvalid;
    wire                                  m_axis_tlast;
    wire                                  mon_valid;
    wire [31:0]                           mon_metric;
    wire signed [31:0]                    mon_cfo_hz;

    // Exposed internal registers (see bootstrap_detector.sby)
    wire [3:0]         state_probe;
    wire [1:0]         idx_probe;
    wire [1:0]         hidx_probe;
    wire [2:0]         win_n_probe;
    wire signed [63:0] r_sum_probe;
    wire               in_det_probe;
    wire               rearm_blocked_probe;
    wire               div_start_probe;
    wire               div_avg_probe;
    wire [63:0]        div_a_divisor_probe;
    wire [47:0]        sample_count_probe;

    bootstrap_detector_bare dut_top (
        .clk                  (clk),
        .rst                  (rst),
        .cfg_sample_rate_hz   (cfg_sample_rate_hz),
        .cfg_threshold_q15    (cfg_threshold_q15),
        .cfg_averaging_window (cfg_averaging_window),
        .cfg_window_clamped   (cfg_window_clamped),
        .s_axis_tdata         (s_axis_tdata),
        .s_axis_tvalid        (s_axis_tvalid),
        .s_axis_tready        (s_axis_tready),
        .s_axis_tlast         (s_axis_tlast),
        .m_axis_tdata         (m_axis_tdata),
        .m_axis_tvalid        (m_axis_tvalid),
        .m_axis_tready        (m_axis_tready),
        .m_axis_tlast         (m_axis_tlast),
        .mon_valid            (mon_valid),
        .mon_metric           (mon_metric),
        .mon_cfo_hz           (mon_cfo_hz),
        .\dut.state           (state_probe),
        .\dut.idx             (idx_probe),
        .\dut.hidx            (hidx_probe),
        .\dut.win_n           (win_n_probe),
        .\dut.r_sum           (r_sum_probe),
        .\dut.in_det          (in_det_probe),
        .\dut.rearm_blocked   (rearm_blocked_probe),
        .\dut.div_start       (div_start_probe),
        .\dut.div_avg         (div_avg_probe),
        .\dut.div_a_divisor   (div_a_divisor_probe),
        .\dut.sample_count    (sample_count_probe)
    );

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;

    // Reset on the first cycle; free thereafter.
    always @(*) begin
        if (!past_valid) begin
            assume (rst);
        end
    end

    reg                                  prev_rst;
    reg [31:0]                           prev_cfg_window;
    reg [2:0]                            prev_win_n;
    reg                                  prev_m_stall;
    reg [`BOOTSTRAP_DETECTION_WIDTH-1:0] prev_m_tdata;

    //--------------------------------------------------------------------
    // Ghost model: accepted-beat count and one monitor update per beat
    //--------------------------------------------------------------------
    localparam [3:0] ST_ACC = 4'd2;
    wire        in_fire = s_axis_tvalid && s_axis_tready;
    reg  [7:0]  g_count;  // low bits suffice to catch a skipped or repeated count
    reg         g_mon_owed;

    always @(posedge clk) begin
        if (rst) begin
            g_count    <= 8'd0;
            g_mon_owed <= 1'b0;
        end else begin
            if (in_fire) g_count <= g_count + 8'd1;
            if (in_fire) g_mon_owed <= 1'b1;
            else if (mon_valid) g_mon_owed <= 1'b0;
        end
    end

    always @(*) begin
        if (past_valid && !rst) begin
            // sample_count advances in ST_ACC, the cycle after the accept.
            assert (sample_count_probe[7:0] == (state_probe == ST_ACC ? g_count - 8'd1 : g_count));
            // ST_DETECT raises mon_valid on the same edge that returns to
            // ST_IDLE (and re-opens s_axis_tready), so the pulse visible
            // this cycle settles the previous beat's debt.
            if (in_fire) assert (!g_mon_owed || mon_valid);
            if (mon_valid) assert (g_mon_owed);
            if (state_probe == ST_IDLE || state_probe == 4'd0) assert (!g_mon_owed || mon_valid);
        end
    end

    always @(posedge clk) begin
        prev_rst        <= rst;
        prev_cfg_window <= cfg_averaging_window;
        prev_win_n      <= win_n_probe;
        prev_m_stall    <= m_axis_tvalid && !m_axis_tready;
        prev_m_tdata    <= m_axis_tdata;
    end

    always @(posedge clk) begin
        if (past_valid && !rst) begin
            // FSM only occupies defined states.
            assert (state_probe <= ST_EMIT);

            // Correlator/delay-line index bound (L-1; 2047 at full size).
            assert (idx_probe <= HALF_SYMBOL - 1);

            // Effective averaging window is always 1..MAX_WIN, and the
            // history index stays inside it.
            assert (win_n_probe >= 1 && win_n_probe <= MAX_WIN);
            assert (hidx_probe < win_n_probe);

            // The window is latched at reset only; it never changes while
            // running (the C++ history length only changes on set_config()).
            if (!prev_rst) begin
                assert (win_n_probe == prev_win_n);
            end

            // (R >= 0 is true but not assertable here: the energies are
            // products, which bootstrap_detector.sby cuts to free values.)

            // Neither divider ever divides by zero: the P/R divide only
            // starts above the near-silence energy floor (below it the
            // datapath skips straight to the CORDIC), and divider A's
            // divisor (R, or the window length) is nonzero on every start.
            if (div_start_probe) begin
                assert (div_a_divisor_probe != 64'd0);
            end
            if (div_start_probe && !div_avg_probe) begin
                assert (div_a_divisor_probe >= MIN_ENERGY);
            end

            // Detection FSM: never tracking a peak while re-arm is blocked.
            assert (!(in_det_probe && rearm_blocked_probe));

            // Input is accepted only when idle; a detection is presented
            // only from ST_EMIT; never both at once (a pending detection
            // holds off the next sample).
            if (s_axis_tready) begin
                assert (state_probe == ST_IDLE);
            end
            if (m_axis_tvalid) begin
                assert (state_probe == ST_EMIT);
                assert (m_axis_tlast);
            end
            assert (!(s_axis_tready && m_axis_tvalid));

            // AXI4-S master: once presented, a status word is held stable
            // until accepted. Gated on !prev_rst: the shadow registers
            // sample the DUT's uninitialized outputs on the reset cycle.
            if (prev_m_stall && !prev_rst) begin
                assert (m_axis_tvalid);
                assert (m_axis_tdata == prev_m_tdata);
            end
        end
    end

    // A zero averaging window is rejected by clamping to 1 (the golden
    // model's own std::max(1, ...)), latched on the reset cycle.
    always @(posedge clk) begin
        if (past_valid && prev_rst && !rst && prev_cfg_window == 32'd0) begin
            assert (win_n_probe == 3'd1);
            assert (!cfg_window_clamped);
        end
        if (past_valid && prev_rst && !rst && prev_cfg_window > MAX_WIN) begin
            assert (win_n_probe == MAX_WIN);
            assert (cfg_window_clamped);
        end
    end

endmodule
