// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// frame_sync_fsm_formal.v — Formal harness for hdl/rtl/sync/frame_sync_fsm.v
//
// The correlator is abstracted away: corr_done / corr_ratio are free inputs
// constrained only by the handshake contract (one correlation outstanding,
// done a few cycles after start, any 16-bit ratio). Its arithmetic is
// checked bit-exactly against the model in test_frame_correlator_tb.py; the
// RAM and AXI4-S framing around the FSM belong to frame_sync.v. What is
// proved here is the control logic, against a ghost record of each chunk
// taken at proc_start:
//
//   - memory safety: every correlation the FSM starts reads corr_len =
//     fft_size samples at corr_offset, and corr_offset + fft_size never
//     exceeds the chunk length (the RAM holds nothing beyond it);
//   - handshake: at most one correlation outstanding, none after the
//     chunk ends, exactly one proc_done per proc_start, events and
//     correlations only while a chunk is in flight;
//   - the sample counter advances by exactly the chunk length (also for a
//     skipped chunk, which must do nothing else);
//   - legal state transitions per chunk (SEARCHING -> SEARCHING |
//     ACQUIRING, ACQUIRING -> ACQUIRING | LOCKED | SEARCHING, LOCKED ->
//     LOCKED | HOLDOVER, HOLDOVER -> HOLDOVER | LOCKED | SEARCHING);
//   - events: at most one per chunk, only ending LOCKED, always when
//     entering LOCKED, carrying the chunk's first sample index and the
//     frame number at the chunk start;
//   - the frame number advances by one for a LOCKED chunk that emitted an
//     event and for a chunk that stayed in HOLDOVER, and otherwise not;
//   - a bootstrap offset loads the expected boundary and the (saturated)
//     confidence and moves SEARCHING / HOLDOVER to ACQUIRING;
//   - reset returns to the constructed state.
//
// Shrunk configuration (assumes below): preamble 4..6 samples, window up
// to 6, small lock counts, chunks of 1..10 samples; the control flow is
// independent of those magnitudes, the 32/48-bit counters are exercised in
// test_frame_sync.py. The products and quotients (frame_samples, the
// threshold scaling, the confidence blends) are cut: no property depends on
// their values, and test_frame_sync.py checks them against the model.
// The proof is bounded (BMC to the depth in the .sby), not unbounded: the
// state machine carries several wide counters whose relations k-induction
// would need strengthening lemmas for. The cover task reaches every
// transition kind and an event.
//
// Assumptions, each a caller contract:
//   - proc_start / bs_apply only while no chunk is in flight and never
//     together (frame_sync.v issues them from its RX state);
//   - corr_done only while a correlation is outstanding and at least two
//     cycles after corr_start (the real correlator needs >= fft_size);
//   - configuration legal and small as above.
//
// Run: hdl/formal/run_formal.sh frame_sync_fsm

module frame_sync_fsm_formal (
    input wire clk,
    input wire rst
);

    reg [15:0] cfg_fft_size;
    reg [15:0] cfg_cp_length;
    reg [15:0] cfg_symbols_per_subframe;
    reg [15:0] cfg_subframes_per_frame;
    reg signed [15:0] cfg_threshold_q15;
    reg [15:0] cfg_lock_acquire_count;
    reg [15:0] cfg_lock_loss_count;
    reg [15:0] cfg_holdover_frames;
    reg [15:0] cfg_search_window;

    reg         bs_apply;
    reg  [47:0] bs_offset;
    reg  [15:0] bs_conf;

    reg         proc_start;
    reg         proc_skip;
    reg  [31:0] proc_n;
    wire        proc_done;

    wire        corr_start;
    wire [31:0] corr_offset;
    wire [15:0] corr_len;
    reg         corr_done;
    reg  [15:0] corr_ratio;

    wire        ev_valid;
    wire [47:0] ev_sample_index;
    wire [31:0] ev_frame_number;
    wire [15:0] ev_confidence;

    wire [1:0]  mon_state;
    wire [31:0] mon_frame_number;
    wire [47:0] mon_expected_boundary;
    wire [15:0] mon_confidence;
    wire [47:0] mon_samples_to_boundary;
    wire [47:0] mon_sample_counter;

    frame_sync_fsm dut (
        .clk(clk), .rst(rst),
        .cfg_fft_size(cfg_fft_size), .cfg_cp_length(cfg_cp_length),
        .cfg_symbols_per_subframe(cfg_symbols_per_subframe),
        .cfg_subframes_per_frame(cfg_subframes_per_frame),
        .cfg_threshold_q15(cfg_threshold_q15),
        .cfg_lock_acquire_count(cfg_lock_acquire_count),
        .cfg_lock_loss_count(cfg_lock_loss_count),
        .cfg_holdover_frames(cfg_holdover_frames),
        .cfg_search_window(cfg_search_window),
        .bs_apply(bs_apply), .bs_offset(bs_offset), .bs_conf(bs_conf),
        .proc_start(proc_start), .proc_skip(proc_skip), .proc_n(proc_n),
        .proc_done(proc_done),
        .corr_start(corr_start), .corr_offset(corr_offset), .corr_len(corr_len),
        .corr_done(corr_done), .corr_ratio(corr_ratio),
        .ev_valid(ev_valid), .ev_sample_index(ev_sample_index),
        .ev_frame_number(ev_frame_number), .ev_confidence(ev_confidence),
        .mon_state(mon_state), .mon_frame_number(mon_frame_number),
        .mon_expected_boundary(mon_expected_boundary),
        .mon_confidence(mon_confidence),
        .mon_samples_to_boundary(mon_samples_to_boundary),
        .mon_sample_counter(mon_sample_counter)
    );

    localparam [1:0] SEARCHING = 2'd0, ACQUIRING = 2'd1, LOCKED = 2'd2, HOLDOVER = 2'd3;

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;
    always @(*) if (!past_valid) assume (rst);

    //--------------------------------------------------------------------------
    // Ghost record of the chunk in flight
    //--------------------------------------------------------------------------

    reg         g_busy;       // between proc_start and proc_done
    reg [1:0]   g_s0;         // state at proc_start
    reg [47:0]  g_sc0;        // sample counter at proc_start
    reg [31:0]  g_frame0;
    reg [31:0]  g_n;
    reg [15:0]  g_fft;
    reg         g_skip;
    reg         g_wait;       // a correlation is outstanding
    reg [3:0]   g_wait_age;   // cycles since corr_start
    reg         g_event;      // an event has been emitted in this chunk
    reg         g_bs_pending; // bs_apply was accepted last cycle
    reg [1:0]   g_bs_s0;
    reg [47:0]  g_bs_offset;
    reg [15:0]  g_bs_conf;
    reg [31:0]  g_bs_frame;
    reg         g_in_reset;   // rst was high on the previous edge

    always @(posedge clk) begin
        g_in_reset   <= rst;
        g_bs_pending <= 1'b0;
        if (rst) begin
            g_busy     <= 1'b0;
            g_wait     <= 1'b0;
            g_wait_age <= 4'd0;
            g_event    <= 1'b0;
        end else begin
            if (proc_start) begin
                g_busy   <= 1'b1;
                g_s0     <= mon_state;
                g_sc0    <= mon_sample_counter;
                g_frame0 <= mon_frame_number;
                g_n      <= proc_n;
                g_fft    <= cfg_fft_size;
                g_skip   <= proc_skip;
                g_event  <= 1'b0;
            end
            if (proc_done) g_busy <= 1'b0;
            if (ev_valid) g_event <= 1'b1;
            if (corr_start) begin
                g_wait     <= 1'b1;
                g_wait_age <= 4'd0;
            end else if (corr_done) begin
                g_wait <= 1'b0;
            end else if (g_wait && g_wait_age != 4'd15) begin
                g_wait_age <= g_wait_age + 4'd1;
            end
            if (bs_apply) begin
                g_bs_pending <= 1'b1;
                g_bs_s0      <= mon_state;
                g_bs_offset  <= bs_offset;
                g_bs_conf    <= bs_conf;
                g_bs_frame   <= mon_frame_number;
            end
        end
    end

    //--------------------------------------------------------------------------
    // Assumptions (caller contracts, see header)
    //--------------------------------------------------------------------------

    always @(*) begin
        if (past_valid && !rst) begin
            assume (!(proc_start && bs_apply));
            assume (!(proc_start && g_busy));
            assume (!(bs_apply && g_busy));
            assume (proc_n >= 32'd1 && proc_n <= 32'd10);
            assume (!corr_done || (g_wait && g_wait_age >= 4'd2));
            assume (cfg_fft_size >= 16'd4 && cfg_fft_size <= 16'd6);
            assume (cfg_cp_length <= 16'd3);
            assume (cfg_symbols_per_subframe <= 16'd2 && cfg_subframes_per_frame <= 16'd2);
            assume (cfg_lock_acquire_count <= 16'd2);
            assume (cfg_lock_loss_count <= 16'd2);
            assume (cfg_holdover_frames <= 16'd2);
            assume (cfg_search_window <= 16'd6);
        end
    end

    //--------------------------------------------------------------------------
    // Properties
    //--------------------------------------------------------------------------

    wire [32:0] corr_end = {1'b0, corr_offset} + {17'd0, corr_len};

    always @(*) begin
        if (past_valid && !rst) begin
            // Handshake and memory safety.
            if (corr_start) begin
                assert (g_busy);
                assert (!g_skip);
                assert (!g_wait);
                assert (corr_len == g_fft);
                assert (corr_end <= {1'b0, g_n});
            end
            if (ev_valid) assert (g_busy && !g_skip);
            if (proc_done) assert (g_busy);
            if (!g_busy) assert (!corr_start && !ev_valid && !proc_done);

            // Chunk results, checked when the chunk completes.
            if (proc_done) begin
                assert (!g_wait);
                assert (mon_sample_counter == g_sc0 + {16'd0, g_n});
                if (g_skip) begin
                    assert (!g_event);
                    assert (mon_state == g_s0);
                    assert (mon_frame_number == g_frame0);
                end else begin
                    if (g_s0 == SEARCHING) assert (mon_state == SEARCHING || mon_state == ACQUIRING);
                    if (g_s0 == LOCKED) assert (mon_state == LOCKED || mon_state == HOLDOVER);
                    if (g_s0 == HOLDOVER) assert (mon_state != ACQUIRING);
                    if (g_s0 == ACQUIRING) assert (mon_state != HOLDOVER);
                    if (g_s0 == SEARCHING) assert (!g_event);
                    if (mon_state == LOCKED && g_s0 != LOCKED) assert (g_event);
                    if (g_event) assert (mon_state == LOCKED);
                    if (g_s0 == LOCKED && mon_state == HOLDOVER) assert (!g_event);
                    // Frame number: +1 for a tracked frame or a predicted one.
                    if ((g_s0 == LOCKED && g_event) || (g_s0 == HOLDOVER && mon_state == HOLDOVER))
                        assert (mon_frame_number == g_frame0 + 32'd1);
                    else
                        assert (mon_frame_number == g_frame0);
                end
            end

            // Event contents.
            if (ev_valid) begin
                assert (ev_sample_index == g_sc0);
                assert (ev_frame_number == g_frame0);
            end
            if (ev_valid && g_event) assert (1'b0);  // one event per chunk

            // Bootstrap offset takes effect on the next cycle.
            if (g_bs_pending) begin
                assert (mon_expected_boundary == g_bs_offset);
                assert (mon_confidence == ((g_bs_conf > 16'd32767) ? 16'd32767 : g_bs_conf));
                assert (mon_frame_number == g_bs_frame);
                if (g_bs_s0 == SEARCHING || g_bs_s0 == HOLDOVER) assert (mon_state == ACQUIRING);
                else assert (mon_state == g_bs_s0);
            end
        end

        // Reset returns to the constructed state.
        if (past_valid && g_in_reset) begin
            assert (mon_state == SEARCHING);
            assert (mon_frame_number == 32'd0);
            assert (mon_sample_counter == 48'd0);
            assert (mon_expected_boundary == 48'd0);
            assert (mon_confidence == 16'd0);
            assert (!proc_done && !corr_start && !ev_valid);
        end
    end

    //--------------------------------------------------------------------------
    // Non-vacuity
    //--------------------------------------------------------------------------

    always @(posedge clk) begin
        if (past_valid && !rst && proc_done && !g_skip) begin
            cover (g_s0 == SEARCHING && mon_state == ACQUIRING);
            cover (g_s0 == ACQUIRING && mon_state == LOCKED && g_event);
            cover (g_s0 == ACQUIRING && mon_state == SEARCHING);
            cover (g_s0 == LOCKED && mon_state == LOCKED && g_event);
            cover (g_s0 == LOCKED && mon_state == HOLDOVER);
            cover (g_s0 == HOLDOVER && mon_state == LOCKED && g_event);
            cover (g_s0 == HOLDOVER && mon_state == SEARCHING);
            cover (g_s0 == HOLDOVER && mon_state == HOLDOVER);
        end
        if (past_valid && !rst) begin
            cover (proc_done && g_skip);
            cover (g_bs_pending && g_bs_s0 == HOLDOVER);
        end
    end

endmodule
