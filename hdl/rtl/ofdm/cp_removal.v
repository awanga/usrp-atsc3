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
// cp_length_ = fft_size * cp_fraction / 8192 in the C++
// (compute_cp_length(), lib/ofdm/cp_removal.h). Since fft_size is always
// a multiple of 8192 for the three defined sizes, fft_size/8192 is
// exactly 1, 2, or 4 with no remainder, so this RTL computes the
// identical product as cp_fraction * (fft_size >> 13) -- an exact
// integer multiply, no divider needed anywhere in this block.
//
// Config is validated/clamped and LATCHED while rst is high, the same
// pattern as bootstrap_detector.v's cfg_averaging_window/win_n: the C++
// only ever changes fft_size/cp_fraction via its constructor or
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
// Invalid config is clamped with a sticky flag latched alongside it, the
// same pattern as bootstrap_detector.v's cfg_window_clamped:
//   - cfg_fft_size not one of {8K, 16K, 32K} -> use 8K, flag
//     cfg_fft_size_invalid.
//   - cfg_cp_fraction_numerator > 4096 (the largest defined CpFraction,
//     k4096_8192) -> clamp to 4096, flag cfg_cp_numerator_clamped.
// Both clamps keep cp_length + fft_size within MAX_SYMBOL_LEN by
// construction, which is what the formal proof checks.
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
    input  wire [12:0]  cfg_cp_fraction_numerator, // literal numerator over 8192 (192..4096)
    output reg          cfg_fft_size_invalid,
    output reg          cfg_cp_numerator_clamped,

    `AXI4S_SLAVE(s_axis, `ATSC3_SAMPLE_WIDTH),
    `AXI4S_MASTER(m_axis, `ATSC3_SAMPLE_WIDTH)
);

    // $clog2 is SystemVerilog, not legal under the project's Verilog-2001
    // lint gate (see hdl/synth/lint.sh); a manual ternary in place of it,
    // same as bootstrap_detector.v's hand-chosen widths for its own fixed
    // spec constants.
    localparam CNT_WIDTH = (MAX_SYMBOL_LEN <= 65536) ? 16 : 17;
    localparam [12:0] MAX_CP_NUMERATOR = 13'd4096;  // CpFraction::k4096_8192
    // A bare macro-expanded decimal literal (e.g. `ATSC3_FFT_8K) can't be
    // bit-sliced directly (`` `ATSC3_FFT_8K[CNT_WIDTH-1:0] `` is a syntax
    // error -- verified the hard way); assigning it into a sized
    // localparam first, as here, is an ordinary context-width assignment
    // and hits none of that.
    localparam [CNT_WIDTH-1:0] DEFAULT_FFT_SIZE = `ATSC3_FFT_8K;

    //--------------------------------------------------------------------
    // Config validation / clamping / latching, resolved once per reset
    // (see header comment for why this isn't read live).
    //--------------------------------------------------------------------

    wire fft_size_valid_now = (cfg_fft_size == `ATSC3_FFT_8K) ||
                              (cfg_fft_size == `ATSC3_FFT_16K) ||
                              (cfg_fft_size == `ATSC3_FFT_32K);
    wire cp_numerator_clamped_now = (cfg_cp_fraction_numerator > MAX_CP_NUMERATOR);

    reg [CNT_WIDTH-1:0] fft_size_r;
    reg [12:0]          cp_numerator_r;

    // fft_size_r / 8192 is exactly 1, 2, or 4 for the three valid sizes
    // -- a plain right shift by 13, no remainder.
    wire [2:0]            fft_scale  = fft_size_r[CNT_WIDTH-1:13];  // always 1, 2, or 4
    wire [CNT_WIDTH-1:0]  cp_length  = cp_numerator_r * fft_scale;  // max 4096*4 = 16384
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
            cfg_fft_size_invalid     <= !fft_size_valid_now;
            fft_size_r               <= fft_size_valid_now ? cfg_fft_size[CNT_WIDTH-1:0] :
                                         DEFAULT_FFT_SIZE;

            cfg_cp_numerator_clamped <= cp_numerator_clamped_now;
            cp_numerator_r           <= cp_numerator_clamped_now ? MAX_CP_NUMERATOR :
                                         cfg_cp_fraction_numerator;

            cnt <= {CNT_WIDTH{1'b0}};
        end else if (fire_in) begin
            cnt <= in_last ? {CNT_WIDTH{1'b0}} : cnt + 1'b1;
        end
    end

    // Unused by design: TLAST has no input framing to honor (see header).
    wire unused_ok = &{1'b0, s_axis_tlast, 1'b0};

endmodule
