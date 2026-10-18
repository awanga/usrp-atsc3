// frame_sync_formal.v — Formal harness for hdl/rtl/sync/frame_sync.v
//
// The state machine and the correlator are replaced by contract-obeying
// stubs (frame_sync_stubs.v; each is proved on its own), leaving the chunk
// buffer and AXI4-S framing under test, checked on ports plus a ghost
// count of accepted beats:
//   - s_axis_tready is low from the cycle after a TLAST beat is accepted
//     until the chunk and its event are done (mon_busy), and never high
//     while an event word is waiting on m_axis;
//   - bs_ready only when no beat of a chunk is stored and nothing is busy,
//     and a beat is never accepted in a cycle where bs_valid & bs_ready;
//   - the event output obeys the AXI4-S hold rule (valid and data held
//     until accepted) and carries TLAST;
//   - cfg_fft_size_invalid sets exactly when a chunk ends with an unusable
//     fft_size (below 4 or above the RAM depth) and, like chunk_overflow
//     (a beat arriving with the RAM full), is sticky until reset;
//   - reset clears everything.
// The bit-exact behaviour of the whole block is checked against the model
// in test_frame_sync.py.
//
// Shrunk: ADDR_W=4 (16-sample RAM). Assumption: AXI4-S producer rule on
// s_axis (a presented beat is held until accepted).
//
// Run: hdl/formal/run_formal.sh frame_sync

module frame_sync_formal (
    input wire clk,
    input wire rst
);

    localparam ADDR_W = 4;
    localparam DEPTH  = 1 << ADDR_W;

    reg [15:0] cfg_fft_size;
    reg [15:0] cfg_cp_length;
    reg [15:0] cfg_symbols_per_subframe;
    reg [15:0] cfg_subframes_per_frame;
    reg signed [15:0] cfg_threshold_q15;
    reg [15:0] cfg_lock_acquire_count;
    reg [15:0] cfg_lock_loss_count;
    reg [15:0] cfg_holdover_frames;
    reg [15:0] cfg_search_window;
    wire       cfg_fft_size_invalid;
    wire       chunk_overflow;

    reg         bs_valid;
    wire        bs_ready;
    reg  [47:0] bs_offset;
    reg  [15:0] bs_conf_q15;

    reg  [31:0] s_axis_tdata;
    reg         s_axis_tvalid;
    wire        s_axis_tready;
    reg         s_axis_tlast;

    wire [135:0] m_axis_tdata;
    wire         m_axis_tvalid;
    reg          m_axis_tready;
    wire         m_axis_tlast;

    wire        mon_busy;
    wire [1:0]  mon_state;
    wire [31:0] mon_frame_number;
    wire [47:0] mon_expected_boundary;
    wire [15:0] mon_confidence;
    wire [47:0] mon_samples_to_boundary;
    wire [47:0] mon_sample_counter;

    frame_sync #(.ADDR_W(ADDR_W)) dut (
        .clk(clk), .rst(rst),
        .cfg_fft_size(cfg_fft_size), .cfg_cp_length(cfg_cp_length),
        .cfg_symbols_per_subframe(cfg_symbols_per_subframe),
        .cfg_subframes_per_frame(cfg_subframes_per_frame),
        .cfg_threshold_q15(cfg_threshold_q15),
        .cfg_lock_acquire_count(cfg_lock_acquire_count),
        .cfg_lock_loss_count(cfg_lock_loss_count),
        .cfg_holdover_frames(cfg_holdover_frames),
        .cfg_search_window(cfg_search_window),
        .cfg_fft_size_invalid(cfg_fft_size_invalid), .chunk_overflow(chunk_overflow),
        .bs_valid(bs_valid), .bs_ready(bs_ready), .bs_offset(bs_offset),
        .bs_conf_q15(bs_conf_q15),
        .s_axis_tdata(s_axis_tdata), .s_axis_tvalid(s_axis_tvalid),
        .s_axis_tready(s_axis_tready), .s_axis_tlast(s_axis_tlast),
        .m_axis_tdata(m_axis_tdata), .m_axis_tvalid(m_axis_tvalid),
        .m_axis_tready(m_axis_tready), .m_axis_tlast(m_axis_tlast),
        .mon_busy(mon_busy), .mon_state(mon_state),
        .mon_frame_number(mon_frame_number),
        .mon_expected_boundary(mon_expected_boundary),
        .mon_confidence(mon_confidence),
        .mon_samples_to_boundary(mon_samples_to_boundary),
        .mon_sample_counter(mon_sample_counter)
    );

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;
    always @(*) if (!past_valid) assume (rst);

    wire fire     = s_axis_tvalid && s_axis_tready;
    wire last_hit = fire && s_axis_tlast;
    wire bad_fft  = (cfg_fft_size < 16'd4) || ({16'd0, cfg_fft_size} > DEPTH);

    // Ghost: beats stored for the chunk being collected (saturating).
    reg [7:0] g_n;
    reg       g_in_reset;
    reg       g_prev_last;       // a TLAST beat was accepted last cycle
    reg       g_prev_stall;      // event valid & not ready last cycle
    reg [135:0] g_prev_word;
    reg       g_prev_bad;
    reg       g_prev_over;
    reg       g_prev_flag_bad;
    reg       g_prev_flag_over;
    reg       g_prev_stream_hold;
    reg [31:0] g_prev_data;
    reg       g_prev_tlast;

    always @(posedge clk) begin
        g_in_reset         <= rst;
        g_prev_last        <= last_hit && !rst;
        g_prev_stall       <= m_axis_tvalid && !m_axis_tready && !rst;
        g_prev_word        <= m_axis_tdata;
        g_prev_bad         <= last_hit && bad_fft && !rst;
        g_prev_over        <= fire && (g_n >= DEPTH) && !rst;
        g_prev_flag_bad    <= cfg_fft_size_invalid;
        g_prev_flag_over   <= chunk_overflow;
        g_prev_stream_hold <= s_axis_tvalid && !s_axis_tready && !rst;
        g_prev_data        <= s_axis_tdata;
        g_prev_tlast       <= s_axis_tlast;
        if (rst) g_n <= 8'd0;
        else if (last_hit) g_n <= 8'd0;
        else if (fire && g_n != 8'd255) g_n <= g_n + 8'd1;
    end

    // Producer rule on s_axis.
    always @(*) begin
        if (past_valid && !rst && g_prev_stream_hold) begin
            assume (s_axis_tvalid);
            assume (s_axis_tdata == g_prev_data);
            assume (s_axis_tlast == g_prev_tlast);
        end
    end

    always @(*) begin
        if (past_valid && !rst) begin
            // Framing.
            if (s_axis_tready) begin
                assert (!mon_busy);
                assert (!m_axis_tvalid);
            end
            if (mon_busy) assert (!bs_ready);
            if (bs_ready) assert (g_n == 8'd0 && !mon_busy);
            if (bs_valid && bs_ready) assert (!s_axis_tready);
            if (g_prev_last) assert (mon_busy);

            // Event output.
            if (m_axis_tvalid) assert (m_axis_tlast);
            if (g_prev_stall) begin
                assert (m_axis_tvalid);
                assert (m_axis_tdata == g_prev_word);
            end

            // Flags.
            if (!g_in_reset) begin
                if (g_prev_flag_bad) assert (cfg_fft_size_invalid);
                if (g_prev_flag_over) assert (chunk_overflow);
                if (g_prev_bad) assert (cfg_fft_size_invalid);
                if (g_prev_over) assert (chunk_overflow);
                if (!g_prev_flag_bad && !g_prev_bad) assert (!cfg_fft_size_invalid);
                if (!g_prev_flag_over && !g_prev_over) assert (!chunk_overflow);
            end
        end
        if (past_valid && g_in_reset) begin
            assert (!mon_busy && !m_axis_tvalid);
            assert (!cfg_fft_size_invalid && !chunk_overflow);
        end
    end

    always @(posedge clk) begin
        if (past_valid && !rst) begin
            cover (last_hit);
            cover (m_axis_tvalid && m_axis_tready);
            cover (m_axis_tvalid && !m_axis_tready);
            cover (cfg_fft_size_invalid);
            cover (chunk_overflow);
            cover (bs_valid && bs_ready);
        end
    end

endmodule
