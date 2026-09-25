// fft_engine.v — Memory-based in-place radix-2 DIT FFT
//
// Bit-exact port of lib/ofdm/fft_engine.cc's ATSC3_FIXED_POINT
// (FixedPointFftEngine) path: forward transform only. The register map
// (config/hdl_register_map.json:fft_engine) ties DIRECTION to
// "const"/kForward and excludes normalize_inverse entirely -- "Receiver
// is forward-FFT-only in RTL scope; inverse direction is a C++-only
// capability not ported (no transmit path in this receiver)" -- so this
// RTL hardcodes the forward-direction twiddle sign and the C++'s
// unconditional (non-normalizing) output path; there is no cfg_direction
// port. FFT_SIZE is one of {8192, 16384, 32768}: bootstrap correlation
// is always 4K and uses its own dedicated correlator
// (bootstrap_detector.v), never this engine (CLAUDE.md Common
// Pitfalls) -- see axi4s_types.vh's `ATSC3_FFT_4K` comment.
//
// Algorithm (mirrors process()/cooley_tukey_dit() exactly):
//   1. Load: each of FFT_SIZE input samples is written directly to its
//      bit-reversed address in a single 32768-entry, complex-int32
//      ("WideSample") working RAM -- the same widen-once/saturate-once-
//      at-the-boundary policy as the C++ (butterflies accumulate
//      growth across up to 15 stages without per-stage saturation).
//   2. Stage 1..log2(FFT_SIZE): FFT_SIZE/2 butterflies per stage, in
//      place. Within one stage no two butterflies ever touch the same
//      RAM index (a structural property of the DIT network), so this
//      RTL enumerates them with one flat counter (bfly_idx) instead of
//      the C++'s nested k/j loops -- same (idx_even, idx_odd, twiddle)
//      triples, different but equivalent enumeration order; see below
//      for the index algebra.
//   3. Unload: FFT_SIZE samples read out in natural (not bit-reversed)
//      order, saturated to int16 at the I/O boundary, exactly like the
//      C++'s non-normalizing output path.
//
// Shared 16384-entry twiddle ROM (TASKS.md's 9.4 spec): rather than one
// table per FFT_SIZE, a single table sized for the largest size (32768,
// needing FFT_SIZE/2 = 16384 unique twiddle values) serves every size.
// For a stage numbered the same way regardless of FFT_SIZE (m = 2^stage,
// independent of FFT_SIZE -- only the number of stages that run and the
// per-stage group count depend on FFT_SIZE), the C++'s
// twiddles_[j * (FFT_SIZE/m)] and this ROM's rom[j << (15 - stage)]
// compute the identical angle: j * (FFT_SIZE/m) scaled to a 32768-point
// table is j * (FFT_SIZE/m) * (32768/FFT_SIZE) = j * (32768/m) =
// j << (15 - stage) -- the FFT_SIZE-dependent terms cancel exactly.
// Bit-reversal is likewise computed, not stored: reversing all 15 ROM-
// address bits of a counter whose top (15 - log2(FFT_SIZE)) bits are
// always 0 (the counter never reaches FFT_SIZE for a smaller-than-32768
// transform) places the reversed low log2(FFT_SIZE) bits at the *top*
// of the 15-bit result, offset by exactly (15 - log2(FFT_SIZE)) bits
// from where a native log2(FFT_SIZE)-bit reversal would put them -- the
// same shift-cancellation as the ROM addressing above, worked out by
// hand and cross-checked by the formal proof (see fft_engine.sby).
//
// RAM: one 32768-entry, 64-bit-wide (complex int32) true dual-port
// block, both ports usable for either a read or a write each cycle --
// a real (if large) on-chip memory resource, not a modeling shortcut.
// Each butterfly costs 3 cycles (address both operands; multiply-
// accumulate once data is valid; write both results), a streaming R2SDF
// pipeline is explicitly out of scope for this milestone (TASKS.md);
// correctness first, per the same policy bootstrap_detector.v and
// timing_recovery.v document (9.17 timing-closure work).
//
// Config (FFT_SIZE) is validated, clamped, and latched at reset, the
// same cp_removal.v/timing_recovery.v pattern and the same reason (the
// C++ has no notion of reconfiguring size mid-transform to match).
//
// AXI4-S: TDATA=ci16 in and out. TLAST out marks the last sample of
// each FFT_SIZE-sample output block (matching the software AXI4-S
// comment's "TLAST(symbol boundary)"); TLAST in is ignored -- like the
// other RTL blocks, this one derives symbol boundaries from its own
// sample counter rather than trusting an upstream TLAST, and the golden
// model (a plain fixed-size array call) has no input framing to honor
// anyway.

`include "axi4s_types.vh"

module fft_engine (
    input  wire         clk,
    input  wire         rst,  // synchronous, active-high

    input  wire [17:0]  cfg_fft_size,  // literal sample count: 8192/16384/32768
    output reg           cfg_fft_size_invalid,

    `AXI4S_SLAVE(s_axis, `ATSC3_SAMPLE_WIDTH),
    `AXI4S_MASTER(m_axis, `ATSC3_SAMPLE_WIDTH)
);

    localparam ADDR_WIDTH = 15;  // log2(32768)
    localparam MAX_N      = 32768;

    //--------------------------------------------------------------------
    // Config validation / clamping / latching (see header)
    //--------------------------------------------------------------------

    wire fft_size_valid_now = (cfg_fft_size == `ATSC3_FFT_8K) ||
                              (cfg_fft_size == `ATSC3_FFT_16K) ||
                              (cfg_fft_size == `ATSC3_FFT_32K);
    localparam [ADDR_WIDTH:0] DEFAULT_FFT_SIZE = `ATSC3_FFT_8K;

    reg [ADDR_WIDTH:0] fft_size_r;  // 8192/16384/32768, needs 16 bits (up to 32768)
    reg [3:0]           log2_n_r;    // 13, 14, or 15

    //--------------------------------------------------------------------
    // Twiddle ROM: 16384 entries, Q1.15 {cos, -sin} (forward direction
    // only -- see header). Generated the same way as
    // timing_recovery_coeffs.vh: dumped from a tool that calls the real
    // atsc3::float_to_q15() on the exact compute_twiddles() formula
    // (sign = -1, forward), not a from-scratch reimplementation of
    // float_to_q15 itself. See hdl/sim/golden/fft_twiddle_gen.cc.
    //--------------------------------------------------------------------

    reg signed [15:0] twiddle_rom_re [0:16383];
    reg signed [15:0] twiddle_rom_im [0:16383];
`ifndef FORMAL_SKIP_ROM_INIT
    initial begin
        `include "fft_twiddles.vh"
    end
`endif

    //--------------------------------------------------------------------
    // Work RAM: 32768 x complex-int32 ("WideSample"), true dual port.
    //--------------------------------------------------------------------

    reg signed [31:0] work_re [0:MAX_N-1];
    reg signed [31:0] work_im [0:MAX_N-1];

    reg                        ram_we_a, ram_we_b;
    reg  [ADDR_WIDTH-1:0]      ram_addr_a, ram_addr_b;
    reg  signed [31:0]         ram_wdata_a_re, ram_wdata_a_im;
    reg  signed [31:0]         ram_wdata_b_re, ram_wdata_b_im;
    reg  signed [31:0]         ram_rdata_a_re, ram_rdata_a_im;
    reg  signed [31:0]         ram_rdata_b_re, ram_rdata_b_im;

    always @(posedge clk) begin
        if (ram_we_a) begin
            work_re[ram_addr_a] <= ram_wdata_a_re;
            work_im[ram_addr_a] <= ram_wdata_a_im;
        end
        ram_rdata_a_re <= work_re[ram_addr_a];
        ram_rdata_a_im <= work_im[ram_addr_a];

        if (ram_we_b) begin
            work_re[ram_addr_b] <= ram_wdata_b_re;
            work_im[ram_addr_b] <= ram_wdata_b_im;
        end
        ram_rdata_b_re <= work_re[ram_addr_b];
        ram_rdata_b_im <= work_im[ram_addr_b];
    end

    //--------------------------------------------------------------------
    // Bit-reversal (load address) and butterfly index algebra (see
    // header for the derivations)
    //--------------------------------------------------------------------

    reg  [ADDR_WIDTH-1:0] sample_cnt;   // load/unload counter, 0..fft_size_r-1
    wire [ADDR_WIDTH-1:0] bitrev_full = {sample_cnt[0], sample_cnt[1], sample_cnt[2],
                                         sample_cnt[3], sample_cnt[4], sample_cnt[5],
                                         sample_cnt[6], sample_cnt[7], sample_cnt[8],
                                         sample_cnt[9], sample_cnt[10], sample_cnt[11],
                                         sample_cnt[12], sample_cnt[13], sample_cnt[14]};
    wire [ADDR_WIDTH-1:0] bitrev_addr  = bitrev_full >> (ADDR_WIDTH[3:0] - log2_n_r);

    reg  [3:0]             stage;       // 1..log2_n_r
    reg  [ADDR_WIDTH-2:0]  bfly_idx;    // 0..(fft_size_r/2 - 1), 14 bits
    wire [3:0]             stage_m1 = stage - 4'd1;  // stage - 1
    wire [ADDR_WIDTH-2:0]  bfly_mask = (14'd1 << stage_m1) - 14'd1;
    wire [ADDR_WIDTH-2:0]  j_idx     = bfly_idx & bfly_mask;
    wire [ADDR_WIDTH-1:0]  group_idx = {1'b0, bfly_idx} >> stage_m1;
    wire [ADDR_WIDTH-1:0]  k_idx     = group_idx << stage;
    wire [ADDR_WIDTH-1:0]  idx_even  = k_idx + {1'b0, j_idx};
    wire [ADDR_WIDTH-1:0]  idx_odd   = idx_even + (15'd1 << stage_m1);
    // Always fits in 14 bits (proven in the header comment: max value
    // 2^14 - 2^(15-stage) <= 16383), so a 14-bit context for the shift
    // (matching j_idx's own width, no extra padding bit) is lossless.
    wire [13:0]            tw_addr   = j_idx << (4'd15 - stage);

    // fft_size_r/2 - 1: fft_size_r >> 1 is 16384 for the largest
    // fft_size_r (32768), one bit wider than last_bfly_idx's 14 bits,
    // but subtracting 1 always brings the *result* back into 14 bits
    // (16383) -- including in that one case, where the intermediate
    // 16384 - 1 and a 14-bit-truncated-then-wrapped 0 - 1 land on the
    // same value (16384 = 0 mod 2^14), so the explicit low-14-bits slice
    // below is exact, not just a convenient truncation.
    wire [ADDR_WIDTH-1:0] half_minus_1_wide = fft_size_r[ADDR_WIDTH:1] - 15'd1;
    wire [ADDR_WIDTH-2:0] last_bfly_idx = half_minus_1_wide[ADDR_WIDTH-2:0];

    //--------------------------------------------------------------------
    // Complex multiply + butterfly (combinational, from the two RAM
    // read-data registers and the ROM)
    //--------------------------------------------------------------------

    wire signed [15:0] tw_re = twiddle_rom_re[tw_addr];
    wire signed [15:0] tw_im = twiddle_rom_im[tw_addr];

    // Same int64 headroom as the C++: odd can be ~2^31, twiddle ~2^15.
    wire signed [63:0] t_re_wide = ($signed(tw_re) * ram_rdata_b_re) -
                                   ($signed(tw_im) * ram_rdata_b_im);
    wire signed [63:0] t_im_wide = ($signed(tw_re) * ram_rdata_b_im) +
                                   ($signed(tw_im) * ram_rdata_b_re);
    // 15 = ATSC3_Q_FRACTIONAL_BITS (lib/types.h); no Verilog macro for it
    // exists yet (only a C++ one), so this is the same literal-15
    // convention bootstrap_detector.v/timing_recovery.v already use for
    // Q1.15 rescale shifts.
    // static_cast<int32_t>(wide_value) in the C++ is a defined low-32-bit
    // truncation of the already-rescaled value; the explicit slice below
    // is the same operation made lint-clean instead of relying on
    // implicit assignment truncation.
    wire signed [63:0] t_re_shifted = t_re_wide >>> 15;
    wire signed [63:0] t_im_shifted = t_im_wide >>> 15;
    wire signed [31:0] t_re = t_re_shifted[31:0];
    wire signed [31:0] t_im = t_im_shifted[31:0];

    wire signed [31:0] even_re = ram_rdata_a_re;
    wire signed [31:0] even_im = ram_rdata_a_im;
    wire signed [31:0] new_even_re = even_re + t_re;
    wire signed [31:0] new_even_im = even_im + t_im;
    wire signed [31:0] new_odd_re  = even_re - t_re;
    wire signed [31:0] new_odd_im  = even_im - t_im;

    //--------------------------------------------------------------------
    // Output saturation (no inverse-normalize path -- forward only)
    //--------------------------------------------------------------------

    function signed [15:0] saturate_i16;
        input signed [31:0] v;
        begin
            if (v > 32'sd32767)
                saturate_i16 = 16'sd32767;
            else if (v < -32'sd32767)
                saturate_i16 = -16'sd32767;
            else
                saturate_i16 = v[15:0];
        end
    endfunction

    //--------------------------------------------------------------------
    // Control FSM
    //--------------------------------------------------------------------

    localparam [3:0] ST_IDLE        = 4'd0,
                     ST_LOAD        = 4'd1,
                     ST_BFLY_ADDR   = 4'd2,
                     ST_BFLY_READ   = 4'd3,
                     ST_BFLY_WRITE  = 4'd4,
                     ST_UNLOAD_ADDR = 4'd5,
                     ST_UNLOAD_WAIT = 4'd6,
                     ST_UNLOAD_EMIT = 4'd7;

    reg [3:0]          state;
    reg signed [15:0]  x_re, x_im;

    always @(posedge clk) begin
        if (rst) begin
            cfg_fft_size_invalid <= !fft_size_valid_now;
            // cfg_fft_size is 18 bits (matching cp_removal.v's port
            // convention) but every valid value (8192/16384/32768) fits
            // in fft_size_r's 16 bits; fft_size_valid_now already
            // guarantees the dropped top 2 bits are 0 whenever this
            // slice is actually selected.
            fft_size_r            <= fft_size_valid_now ? cfg_fft_size[ADDR_WIDTH:0] : DEFAULT_FFT_SIZE;
            if (cfg_fft_size == `ATSC3_FFT_16K) begin
                log2_n_r <= 4'd14;
            end else if (cfg_fft_size == `ATSC3_FFT_32K) begin
                log2_n_r <= 4'd15;
            end else begin
                log2_n_r <= 4'd13;  // 8K, and the invalid-config fallback
            end

            state      <= ST_IDLE;
            sample_cnt <= {ADDR_WIDTH{1'b0}};
            stage      <= 4'd1;
            bfly_idx   <= {(ADDR_WIDTH-1){1'b0}};

            ram_we_a   <= 1'b0;
            ram_we_b   <= 1'b0;
            ram_addr_a <= {ADDR_WIDTH{1'b0}};
            ram_addr_b <= {ADDR_WIDTH{1'b0}};

            s_axis_tready <= 1'b0;
            m_axis_tvalid <= 1'b0;
            m_axis_tdata  <= {`ATSC3_SAMPLE_WIDTH{1'b0}};
            m_axis_tlast  <= 1'b0;
        end else begin
            ram_we_a <= 1'b0;
            ram_we_b <= 1'b0;

            case (state)
                ST_IDLE: begin
                    s_axis_tready <= 1'b1;
                    sample_cnt    <= {ADDR_WIDTH{1'b0}};
                    state         <= ST_LOAD;
                end

                ST_LOAD: begin
                    if (s_axis_tvalid && s_axis_tready) begin
                        ram_we_a       <= 1'b1;
                        ram_addr_a     <= bitrev_addr;
                        ram_wdata_a_re <= {{16{s_axis_tdata[31]}}, s_axis_tdata[31:16]};
                        ram_wdata_a_im <= {{16{s_axis_tdata[15]}}, s_axis_tdata[15:0]};

                        if (sample_cnt == fft_size_r[ADDR_WIDTH-1:0] - 1'b1) begin
                            s_axis_tready <= 1'b0;
                            stage         <= 4'd1;
                            bfly_idx      <= {(ADDR_WIDTH-1){1'b0}};
                            state         <= ST_BFLY_ADDR;
                        end else begin
                            sample_cnt <= sample_cnt + 1'b1;
                        end
                    end
                end

                ST_BFLY_ADDR: begin
                    ram_addr_a <= idx_even;
                    ram_addr_b <= idx_odd;
                    state      <= ST_BFLY_READ;
                end

                ST_BFLY_READ: begin
                    // ram_rdata_a/b now reflect idx_even/idx_odd (one
                    // cycle of RAM read latency after ST_BFLY_ADDR).
                    state <= ST_BFLY_WRITE;
                end

                ST_BFLY_WRITE: begin
                    ram_we_a       <= 1'b1;
                    ram_wdata_a_re <= new_even_re;
                    ram_wdata_a_im <= new_even_im;
                    ram_we_b       <= 1'b1;
                    ram_wdata_b_re <= new_odd_re;
                    ram_wdata_b_im <= new_odd_im;
                    // ram_addr_a/b are unchanged from ST_BFLY_ADDR
                    // (idx_even/idx_odd) -- write back to the same pair.

                    if (bfly_idx == last_bfly_idx) begin
                        if (stage == log2_n_r) begin
                            sample_cnt <= {ADDR_WIDTH{1'b0}};
                            state      <= ST_UNLOAD_ADDR;
                        end else begin
                            stage    <= stage + 1'b1;
                            bfly_idx <= {(ADDR_WIDTH-1){1'b0}};
                            state    <= ST_BFLY_ADDR;
                        end
                    end else begin
                        bfly_idx <= bfly_idx + 1'b1;
                        state    <= ST_BFLY_ADDR;
                    end
                end

                // Every sample goes through the same 3-cycle ADDR/WAIT/
                // EMIT sequence as a butterfly's ADDR/READ/WRITE -- an
                // address needs a full settled cycle before
                // ram_rdata_a reflects it. An earlier version of this
                // loop set up sample_cnt+1's address from inside
                // ST_UNLOAD_EMIT and looped straight back into itself
                // with no settling cycle in between (correct only for
                // the *first* sample, whose ST_UNLOAD_ADDR->ST_UNLOAD_
                // WAIT->ST_UNLOAD_EMIT path does have one), silently
                // emitting sample_cnt-1's data as sample_cnt for every
                // sample after the first. Caught by test_fft_engine.py's
                // random scenarios, traced to ground with a hand-
                // computed small-N (N=8) reference after the first,
                // narrower fix (which only added the missing cycle to
                // the first sample) turned "everything shifted by one"
                // into "index 0 is right, index 1 duplicates it, then
                // shifted by one again from there" -- the signature of
                // the same missing-cycle bug still present in the loop.
                ST_UNLOAD_ADDR: begin
                    ram_addr_a <= sample_cnt;
                    state      <= ST_UNLOAD_WAIT;
                end

                ST_UNLOAD_WAIT: begin
                    state <= ST_UNLOAD_EMIT;
                end

                ST_UNLOAD_EMIT: begin
                    if (!m_axis_tvalid || m_axis_tready) begin
                        x_re          <= saturate_i16(ram_rdata_a_re);
                        x_im          <= saturate_i16(ram_rdata_a_im);
                        m_axis_tdata  <= {saturate_i16(ram_rdata_a_re), saturate_i16(ram_rdata_a_im)};
                        m_axis_tvalid <= 1'b1;
                        m_axis_tlast  <= (sample_cnt == fft_size_r[ADDR_WIDTH-1:0] - 1'b1);

                        if (sample_cnt == fft_size_r[ADDR_WIDTH-1:0] - 1'b1) begin
                            state <= ST_IDLE;
                        end else begin
                            sample_cnt <= sample_cnt + 1'b1;
                            state      <= ST_UNLOAD_ADDR;
                        end
                    end
                end

                default: state <= ST_IDLE;
            endcase

            // Output handshake: once accepted, drop tvalid unless the
            // next beat is already queued up above this cycle.
            if (m_axis_tvalid && m_axis_tready && state != ST_UNLOAD_EMIT) begin
                m_axis_tvalid <= 1'b0;
            end
        end
    end

    // Unused by design: s_axis_tlast has no input framing to honor (see
    // header); x_re/x_im are latched for waveform-debug visibility only,
    // m_axis_tdata is driven directly from the saturate function.
    wire unused_ok = &{1'b0, s_axis_tlast, x_re, x_im, half_minus_1_wide[14],
                       t_re_shifted[63:32], t_im_shifted[63:32], 1'b0};

endmodule
