// cp_removal_bare.v — Trivial single-instance wrapper, used only to seed
// the flatten+expose step in cp_removal.sby's [script] that promotes
// cp_removal's internal cnt/cp_length/symbol_len/in_cp registers and
// wires to top-level ports so the formal harness can reference them (see
// hdl/docs/formal_conventions.md and axi4s_skid_buffer_bare.v for why
// this replaces a `bind`-based checker).
//
// No parameter override: MAX_SYMBOL_LEN stays at cp_removal.v's real-spec
// default (CNT_WIDTH = 16). Unlike bootstrap_detector's 64-bit dividers
// and correlation products, this block's only arithmetic is a 13x3-bit
// multiply and a 16-bit counter compare -- small enough for smtbmc/z3 to
// handle directly at full width, so there is no need to shrink it (see
// cp_removal.sby).
//
// Verification-only. Not synthesized, not part of rtl/.

module cp_removal_bare (
    input  wire         clk,
    input  wire         rst,

    input  wire [17:0]  cfg_fft_size,
    input  wire [12:0]  cfg_cp_fraction_numerator,
    output wire         cfg_fft_size_invalid,
    output wire         cfg_cp_numerator_clamped,

    input  wire [31:0]  s_axis_tdata,
    input  wire         s_axis_tvalid,
    output wire         s_axis_tready,
    input  wire         s_axis_tlast,

    output wire [31:0]  m_axis_tdata,
    output wire         m_axis_tvalid,
    input  wire         m_axis_tready,
    output wire         m_axis_tlast
);

    cp_removal dut (
        .clk                       (clk),
        .rst                       (rst),
        .cfg_fft_size              (cfg_fft_size),
        .cfg_cp_fraction_numerator (cfg_cp_fraction_numerator),
        .cfg_fft_size_invalid      (cfg_fft_size_invalid),
        .cfg_cp_numerator_clamped  (cfg_cp_numerator_clamped),
        .s_axis_tdata              (s_axis_tdata),
        .s_axis_tvalid             (s_axis_tvalid),
        .s_axis_tready             (s_axis_tready),
        .s_axis_tlast              (s_axis_tlast),
        .m_axis_tdata              (m_axis_tdata),
        .m_axis_tvalid             (m_axis_tvalid),
        .m_axis_tready             (m_axis_tready),
        .m_axis_tlast              (m_axis_tlast)
    );

endmodule
