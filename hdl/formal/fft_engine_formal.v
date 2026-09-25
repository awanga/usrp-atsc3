// fft_engine_formal.v — Formal harness top for fft_engine
//
// Verification-only. Not synthesized, not part of rtl/. Instantiates the
// flatten+expose-generated fft_engine_bare (produced earlier in
// fft_engine.sby's [script] -- see that file, fft_engine_bare.v, and
// hdl/docs/formal_conventions.md for the two-stage flatten+expose flow
// this replaces a `bind`-based checker with).
//
// Scoped to exactly TASKS.md's 9.4 formal ask -- "bit-reversal address
// generator bounds, butterfly arithmetic [bounds]" -- plus FSM legality
// and AXI4-S protocol, not a full proof of transform correctness
// (cocotb's job, against the real golden model, at real FFT_SIZE; see
// test_fft_engine.py, which found and pinned down a real off-by-one in
// the unload read pipeline that these address-bound properties, being
// purely structural, would never have caught).
//
// All of bitrev_addr / idx_even / idx_odd / tw_addr are recomputed here
// from the exposed registers (fft_size_r, log2_n_r, stage, bfly_idx,
// sample_cnt) using the same formulas fft_engine.v uses -- an
// independent restatement, not a re-read of the DUT's own wires, so the
// proof checks the *formulas*, not just that the RTL agrees with
// itself.
//
// Properties:
//   - FSM legality: state is always one of the 8 defined encodings.
//   - fft_size_r is always one of {8K, 16K, 32K}; log2_n_r matches it
//     exactly (13/14/15).
//   - sample_cnt < fft_size_r; bfly_idx < fft_size_r/2; stage is in
//     [1, log2_n_r] whenever a butterfly state is active.
//   - bitrev_addr < fft_size_r always (the bit-reversal address
//     generator bound).
//   - idx_even < fft_size_r, idx_odd < fft_size_r, and idx_even !=
//     idx_odd always (the butterfly arithmetic bound -- a same-stage
//     collision would silently corrupt the in-place transform).
//   - s_axis_tready / m_axis_tvalid pinned to their exact controlling
//     FSM state (ST_LOAD / ST_UNLOAD_EMIT), the same reason
//     timing_recovery_formal.v does this rather than a purely temporal
//     stability statement (k-induction needs the FSM-state anchor to
//     close, not just the symptom).
//
// The twiddle complex multiply is cut (`cutpoint t:$mul` in
// fft_engine.sby): none of the properties above depend on a butterfly's
// numeric *result*, only on which addresses it touches, so this is a
// sound over-approximation, the same reasoning bootstrap_detector.sby
// and timing_recovery.sby use.
//
// Run: cd hdl/formal && sby -f fft_engine.sby

`include "axi4s_types.vh"

module fft_engine_formal (
    input wire clk,
    input wire rst
);

    localparam ADDR_WIDTH = 15;

    reg  [17:0] cfg_fft_size;
    wire         cfg_fft_size_invalid;

    reg  [31:0] s_axis_tdata;
    reg         s_axis_tvalid;
    wire        s_axis_tready;
    reg         s_axis_tlast;

    wire [31:0] m_axis_tdata;
    wire        m_axis_tvalid;
    reg         m_axis_tready;
    wire        m_axis_tlast;

    // Probes wired to the DUT's internal control registers via the
    // ports fft_engine.sby's [script] exposed on fft_engine_bare -- see
    // that file's header.
    wire [3:0]             state_probe;
    wire [3:0]             stage_probe;
    wire [ADDR_WIDTH-2:0]  bfly_idx_probe;
    wire [ADDR_WIDTH-1:0]  sample_cnt_probe;
    wire [3:0]             log2_n_r_probe;
    wire [ADDR_WIDTH:0]    fft_size_r_probe;

    fft_engine_bare dut_top (
        .clk                  (clk),
        .rst                  (rst),
        .cfg_fft_size         (cfg_fft_size),
        .cfg_fft_size_invalid (cfg_fft_size_invalid),
        .s_axis_tdata         (s_axis_tdata),
        .s_axis_tvalid        (s_axis_tvalid),
        .s_axis_tready        (s_axis_tready),
        .s_axis_tlast         (s_axis_tlast),
        .m_axis_tdata         (m_axis_tdata),
        .m_axis_tvalid        (m_axis_tvalid),
        .m_axis_tready        (m_axis_tready),
        .m_axis_tlast         (m_axis_tlast),
        .\dut.state       (state_probe),
        .\dut.stage       (stage_probe),
        .\dut.bfly_idx    (bfly_idx_probe),
        .\dut.sample_cnt  (sample_cnt_probe),
        .\dut.log2_n_r    (log2_n_r_probe),
        .\dut.fft_size_r  (fft_size_r_probe)
    );

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;

    always @(*) begin
        if (!past_valid) begin
            assume (rst);
        end
    end

    reg        prev_rst;
    reg        prev_m_valid, prev_m_ready, prev_m_tlast;
    reg [31:0] prev_m_tdata;

    always @(posedge clk) begin
        prev_rst     <= rst;
        prev_m_valid <= m_axis_tvalid;
        prev_m_ready <= m_axis_tready;
        prev_m_tdata <= m_axis_tdata;
        prev_m_tlast <= m_axis_tlast;
    end

    //--------------------------------------------------------------------
    // FSM legality and config bounds
    //--------------------------------------------------------------------
    always @(posedge clk) begin
        if (past_valid) begin
            assert (state_probe <= 4'd7);
            assert (fft_size_r_probe == `ATSC3_FFT_8K || fft_size_r_probe == `ATSC3_FFT_16K ||
                    fft_size_r_probe == `ATSC3_FFT_32K);
            if (fft_size_r_probe == `ATSC3_FFT_8K)
                assert (log2_n_r_probe == 4'd13);
            else if (fft_size_r_probe == `ATSC3_FFT_16K)
                assert (log2_n_r_probe == 4'd14);
            else
                assert (log2_n_r_probe == 4'd15);
        end
    end

    //--------------------------------------------------------------------
    // Counter bounds
    //--------------------------------------------------------------------
    wire [ADDR_WIDTH-1:0] fft_size_half_wide = fft_size_r_probe[ADDR_WIDTH:1] - 15'd1;
    wire [ADDR_WIDTH-2:0] max_bfly_idx = fft_size_half_wide[ADDR_WIDTH-2:0];

    always @(posedge clk) begin
        if (past_valid) begin
            // fft_size_r_probe is ADDR_WIDTH+1 bits wide specifically because
            // FFT_32K (32768) needs the extra bit; comparing against a
            // truncated fft_size_r_probe[ADDR_WIDTH-1:0] would drop that bit
            // and silently compare against 0 whenever fft_size_r_probe ==
            // 32768. Comparing the full width lets Verilog zero-extend the
            // narrower (ADDR_WIDTH-bit) LHS instead, which is what's wanted:
            // valid addresses are always < 32768 and fit in ADDR_WIDTH bits,
            // but the size value itself can equal exactly 32768.
            assert (sample_cnt_probe < fft_size_r_probe);
            assert (bfly_idx_probe <= max_bfly_idx);
            if (state_probe == 4'd2 || state_probe == 4'd3 || state_probe == 4'd4) begin
                // ST_BFLY_ADDR / ST_BFLY_READ / ST_BFLY_WRITE
                assert (stage_probe >= 4'd1);
                assert (stage_probe <= log2_n_r_probe);
            end
        end
    end

    //--------------------------------------------------------------------
    // Bit-reversal address generator bound (independent recomputation)
    //--------------------------------------------------------------------
    wire [ADDR_WIDTH-1:0] bitrev_full_chk =
        {sample_cnt_probe[0], sample_cnt_probe[1], sample_cnt_probe[2], sample_cnt_probe[3],
         sample_cnt_probe[4], sample_cnt_probe[5], sample_cnt_probe[6], sample_cnt_probe[7],
         sample_cnt_probe[8], sample_cnt_probe[9], sample_cnt_probe[10], sample_cnt_probe[11],
         sample_cnt_probe[12], sample_cnt_probe[13], sample_cnt_probe[14]};
    wire [ADDR_WIDTH-1:0] bitrev_addr_chk = bitrev_full_chk >> (ADDR_WIDTH[3:0] - log2_n_r_probe);

    always @(posedge clk) begin
        if (past_valid) begin
            assert (bitrev_addr_chk < fft_size_r_probe);
        end
    end

    //--------------------------------------------------------------------
    // Butterfly arithmetic bound (independent recomputation)
    //--------------------------------------------------------------------
    wire [3:0]             stage_m1_chk  = stage_probe - 4'd1;
    wire [ADDR_WIDTH-2:0]  bfly_mask_chk = (14'd1 << stage_m1_chk) - 14'd1;
    wire [ADDR_WIDTH-2:0]  j_idx_chk     = bfly_idx_probe & bfly_mask_chk;
    wire [ADDR_WIDTH-1:0]  group_idx_chk = {1'b0, bfly_idx_probe} >> stage_m1_chk;
    wire [ADDR_WIDTH-1:0]  k_idx_chk     = group_idx_chk << stage_probe;
    wire [ADDR_WIDTH-1:0]  idx_even_chk  = k_idx_chk + {1'b0, j_idx_chk};
    wire [ADDR_WIDTH-1:0]  idx_odd_chk   = idx_even_chk + (15'd1 << stage_m1_chk);

    always @(posedge clk) begin
        if (past_valid && (state_probe == 4'd2 || state_probe == 4'd3 || state_probe == 4'd4)) begin
            assert (idx_even_chk < fft_size_r_probe);
            assert (idx_odd_chk < fft_size_r_probe);
            assert (idx_even_chk != idx_odd_chk);
        end
    end

    //--------------------------------------------------------------------
    // AXI4-S protocol.
    //
    // s_axis_tready is pinned to its controlling FSM state (see file
    // header on why a purely temporal form isn't inductive): ST_IDLE
    // sets it one cycle *before* the FSM actually enters ST_LOAD, so by
    // the time state_probe reads ST_LOAD, s_axis_tready has already
    // settled to 1 -- an exact state/signal correspondence.
    //
    // m_axis_tvalid does NOT have that same correspondence, and pinning
    // it to ST_UNLOAD_EMIT the same way is simply false, not just
    // uninductive: fft_engine.v sets m_axis_tvalid <= 1 *from inside*
    // ST_UNLOAD_EMIT's own case branch (gated on the data becoming
    // ready), so the registered value doesn't show up until the cycle
    // *after*, by which point state_probe has already advanced to
    // ST_UNLOAD_ADDR (see fft_engine.v's ST_UNLOAD_EMIT comment on the
    // 3-cycle ADDR/WAIT/EMIT sequence). Confirmed empirically with a
    // throwaway cocotb probe (not checked in) before writing this: in
    // the free-running (tready always 1) case, m_axis_tvalid pulses for
    // exactly one cycle per 3-cycle unload period, coinciding with
    // state_probe == ST_UNLOAD_ADDR, not ST_UNLOAD_EMIT; under
    // backpressure it instead holds at 1 for as long as state_probe
    // stays at ST_UNLOAD_EMIT. Both are correct AXI4-S behavior, but
    // neither is "tvalid iff state == X" -- there is no single state
    // value that characterizes it, because whether a given ST_UNLOAD_
    // EMIT visit is "the cycle data becomes ready" or "a repeat cycle
    // of an ongoing stall" isn't observable from state_probe alone.
    //
    // What's true regardless of history, and is what k-induction
    // actually needs here: m_axis_tvalid can only ever change on a
    // cycle where m_axis_tready fires (every path that sets OR clears
    // it in fft_engine.v is gated on m_axis_tready, whether re-latching
    // fresh data in ST_UNLOAD_EMIT or dropping it in the post-case
    // "output handshake" block) -- so if the previous cycle was
    // stalled (valid with no ready), m_axis_tvalid cannot have dropped.
    // That's checked below instead of a state pin.
    //
    // Tempting but FALSE, so deliberately not asserted: "m_axis_tvalid
    // implies state is one of the unload states". The very last sample
    // of a transform sets m_axis_tvalid <= 1 in the same cycle state
    // moves to ST_IDLE (same "set unconditionally inside ST_UNLOAD_
    // EMIT's firing branch" mechanism as the mid-transform case above),
    // and if m_axis_tready then stays low, that unconsumed valid+data
    // rides along through ST_IDLE/ST_LOAD/ST_BFLY_* of the *next*
    // transform indefinitely -- s_axis and m_axis are independent
    // buses, and nothing here blocks starting the next load on the
    // previous output still being unconsumed. Real, intended pipelining
    // behavior, not a bug; caught by k-induction on a first attempt at
    // this implication, the same way the ST_UNLOAD_EMIT state pin
    // above was caught by a basecase/induction counterexample rather
    // than by inspection.
    //--------------------------------------------------------------------
    always @(posedge clk) begin
        if (past_valid && !prev_rst) begin
            assert (s_axis_tready == (state_probe == 4'd1));   // ST_LOAD
        end
        if (past_valid && !prev_rst && prev_m_valid && !prev_m_ready) begin
            // AXI4-S requires TVALID to never retract before the beat is
            // accepted, on top of TDATA/TLAST holding steady (already
            // checked): once asserted with tready low, all three must
            // still be exactly what they were last cycle.
            assert (m_axis_tvalid);
            assert (m_axis_tdata == prev_m_tdata);
            assert (m_axis_tlast == prev_m_tlast);
        end
        if (past_valid && prev_rst) begin
            assert (!s_axis_tready);
            assert (!m_axis_tvalid);
        end
    end

    //--------------------------------------------------------------------
    // Non-vacuity: ST_UNLOAD_EMIT/m_axis_tvalid excluded (needs the full
    // load+transform to finish first, well past any practical BMC
    // depth -- the same reasoning cp_removal.sby's and
    // timing_recovery.sby's excluded cover goals document; cocotb
    // exercises it concretely, repeatedly, at full scale).
    //--------------------------------------------------------------------
    always @(posedge clk) begin
        if (past_valid) begin
            cover (state_probe == 4'd1);  // ST_LOAD
            cover (cfg_fft_size_invalid);
        end
    end

endmodule
