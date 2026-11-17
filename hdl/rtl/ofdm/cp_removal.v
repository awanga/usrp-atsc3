// SPDX-License-Identifier: CERN-OHL-W-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// cp_removal.v — Cyclic Prefix removal for OFDM symbols
//
// Bit-exact (by construction, not by a numeric equivalence check -- see
// below) port of lib/ofdm/cp_removal.cc: a pure position counter gates
// which input samples reach the output. No arithmetic on TDATA at all,
// so there is nothing for a fixed-point vs. float divergence to hide in;
// the only thing to get right is the framing (which samples pass, which
// TLAST lands on).
//
// The C++ reference (CpRemoval::process()) buffers a whole CP+FFT symbol
// before emitting the FFT-length tail via its callback. This RTL instead
// streams straight through with zero internal buffering: TREADY is held
// high (samples freely accepted and discarded) for the first CP_LENGTH
// samples of each symbol, then wired to TREADY of the next stage for the
// remaining FFT_SIZE samples, with TDATA passed through unchanged and
// TLAST asserted on the last of those FFT_SIZE beats. Both designs emit
// exactly the same sequence of symbol values in the same order --
// process()'s buffering is a software convenience (a single contiguous
// span to hand the callback), not a framing difference -- so the two are
// bit-exact on content while differing in latency, the same kind of
// implementation freedom bootstrap_detector.v takes (a multi-cycle
// divider/CORDIC pipeline vs. the C++'s single-call-per-sample model).
//
// The guard interval is configured by its A/322 Table 9.13 code, as L1
// signals it; its length in samples comes from Table 8.9 and is the same
// for every FFT size (lib/config/atsc3_config.h guard_interval_samples()).
// A 12-entry table, no arithmetic: the length is looked up once, at
// reset, and latched.
//
// Config is validated/defaulted and LATCHED while rst is high, the same
// pattern as bootstrap_detector.v's cfg_averaging_window/win_n: the C++
// only ever changes fft_size/guard_interval via its constructor or
// reconfigure() (which resets buffer_idx_), never mid-symbol, so there is
// no golden-model behavior to match for a live config change anyway.
// Latching also makes "the position counter never exceeds the max
// defined symbol length" (see below) an invariant that holds unqualified
// -- with a live (unlatched) config, an adversarial config change on the
// cycle after a large symbol_len would let the *current* cnt momentarily
// exceed a suddenly-smaller symbol_len with no clock edge in between,
// which isn't a real bug (this block has no RAM/array for such a moment
// to corrupt) but also isn't a provable invariant. Reconfigure by
// resetting the block, same as bootstrap_detector.v's averaging window.
//
// Invalid config falls back to a default with a sticky flag latched
// alongside it, the same pattern as bootstrap_detector.v's
// cfg_window_clamped (the C++ throws instead):
//   - cfg_fft_size not one of {8K, 16K, 32K} -> use 8K, flag
//     cfg_fft_size_invalid.
//   - cfg_guard_interval reserved (0, 13-15) or not allowed with the
//     (possibly defaulted) FFT size by Table 8.9 (GI8-GI11 with 8K, GI12
//     with 8K or 16K) -> use GI5_1024, allowed at every size, flag
//     cfg_guard_interval_invalid.
// Both keep cp_length + fft_size within MAX_SYMBOL_LEN by construction,
// which is what the formal proof checks.
//
// AXI4-S: TDATA=ci16 in and out (Q1.15, pass-through, no arithmetic);
// TLAST out marks the last sample of each FFT-length symbol. TLAST in is
// ignored -- like bootstrap_detector.v, the golden model has no input
// framing of its own.

`include "axi4s_types.vh"

module cp_removal #(
    parameter MAX_SYMBOL_LEN = `ATSC3_FFT_32K + `ATSC3_FFT_32K / 2  // 32768 + 16384 = 49152
) (
    input  wire         clk,
    input  wire         rst,  // synchronous, active-high

    input  wire [17:0]  cfg_fft_size,             // literal sample count: 8192/16384/32768
    input  wire [3:0]   cfg_guard_interval,       // A/322 Table 9.13 code, GI1..GI12 = 1..12
    output reg          cfg_fft_size_invalid,
    output reg          cfg_guard_interval_invalid,

    `AXI4S_SLAVE(s_axis, `ATSC3_SAMPLE_WIDTH),
    `AXI4S_MASTER(m_axis, `ATSC3_SAMPLE_WIDTH)
);

    // $clog2 is not Verilog-2001.
    localparam CNT_WIDTH = (MAX_SYMBOL_LEN <= 65536) ? 16 : 17;
    localparam [12:0] DEFAULT_GI_SAMPLES = 13'd1024;  // GI5_1024
    // A macro-expanded decimal literal can't be bit-sliced directly; a
    // sized localparam gives it this block's counter width.
    localparam [CNT_WIDTH-1:0] DEFAULT_FFT_SIZE = `ATSC3_FFT_8K;

    //--------------------------------------------------------------------
    // Config validation / defaulting / latching, resolved once per reset
    // (see header comment for why this isn't read live).
    //--------------------------------------------------------------------

    wire fft_size_valid_now = (cfg_fft_size == `ATSC3_FFT_8K) ||
                              (cfg_fft_size == `ATSC3_FFT_16K) ||
                              (cfg_fft_size == `ATSC3_FFT_32K);
    wire [CNT_WIDTH-1:0] fft_size_now = fft_size_valid_now ? cfg_fft_size[CNT_WIDTH-1:0] :
                                                             DEFAULT_FFT_SIZE;

    // A/322 Table 8.9 length of the Table 9.13 code; 0 for a reserved code.
    reg [12:0] gi_samples_now;
    always @(*) begin
        case (cfg_guard_interval)
            4'd1:    gi_samples_now = 13'd192;
            4'd2:    gi_samples_now = 13'd384;
            4'd3:    gi_samples_now = 13'd512;
            4'd4:    gi_samples_now = 13'd768;
            4'd5:    gi_samples_now = 13'd1024;
            4'd6:    gi_samples_now = 13'd1536;
            4'd7:    gi_samples_now = 13'd2048;
            4'd8:    gi_samples_now = 13'd2432;
            4'd9:    gi_samples_now = 13'd3072;
            4'd10:   gi_samples_now = 13'd3648;
            4'd11:   gi_samples_now = 13'd4096;
            4'd12:   gi_samples_now = 13'd4864;
            default: gi_samples_now = 13'd0;
        endcase
    end

    // Table 8.9: GI1-GI7 at every size, GI8-GI11 not with 8K, GI12 only
    // with 32K.
    wire gi_allowed_now = (gi_samples_now != 13'd0) &&
                          ((cfg_guard_interval <= 4'd7) ||
                           (cfg_guard_interval <= 4'd11 && fft_size_now != `ATSC3_FFT_8K) ||
                           (fft_size_now == `ATSC3_FFT_32K));

    reg [CNT_WIDTH-1:0] fft_size_r;
    reg [12:0]          cp_length_r;

    wire [CNT_WIDTH-1:0]  cp_length  = {{(CNT_WIDTH-13){1'b0}}, cp_length_r};  // max 4864
    wire [CNT_WIDTH-1:0]  symbol_len = fft_size_r + cp_length;

    //--------------------------------------------------------------------
    // Position counter and streaming datapath
    //--------------------------------------------------------------------

    reg [CNT_WIDTH-1:0] cnt;  // position within the current CP+FFT symbol

    wire in_cp   = (cnt < cp_length);
    wire in_last = (cnt == symbol_len - 1'b1);
    wire fire_in = s_axis_tvalid && s_axis_tready;

    always @(*) begin
        s_axis_tready = in_cp ? 1'b1 : m_axis_tready;
        m_axis_tvalid = !in_cp && s_axis_tvalid;
        m_axis_tdata  = s_axis_tdata;
        m_axis_tlast  = !in_cp && s_axis_tvalid && in_last;
    end

    always @(posedge clk) begin
        if (rst) begin
            cfg_fft_size_invalid       <= !fft_size_valid_now;
            fft_size_r                 <= fft_size_now;

            cfg_guard_interval_invalid <= !gi_allowed_now;
            cp_length_r                <= gi_allowed_now ? gi_samples_now : DEFAULT_GI_SAMPLES;

            cnt <= {CNT_WIDTH{1'b0}};
        end else if (fire_in) begin
            cnt <= in_last ? {CNT_WIDTH{1'b0}} : cnt + 1'b1;
        end
    end

    // Unused by design: TLAST has no input framing to honor (see header).
    wire unused_ok = &{1'b0, s_axis_tlast, 1'b0};

endmodule
