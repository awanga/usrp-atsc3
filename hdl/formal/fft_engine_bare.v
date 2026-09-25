// fft_engine_bare.v — Trivial single-instance wrapper, used only to seed
// the flatten+expose step in fft_engine.sby's [script] that promotes
// fft_engine's internal state/stage/bfly_idx/sample_cnt/log2_n_r/
// fft_size_r registers to top-level ports so the formal harness can
// reference them (see hdl/docs/formal_conventions.md and
// axi4s_skid_buffer_bare.v for why this replaces a `bind`-based
// checker).
//
// No parameter override: this block's only sizable arithmetic (the
// twiddle complex multiply) is cut in fft_engine.sby (sound for the
// address-bound/protocol-only properties proved here; see that file),
// so there's no need to shrink anything for smtbmc/z3.
//
// Verification-only. Not synthesized, not part of rtl/.

module fft_engine_bare (
    input  wire         clk,
    input  wire         rst,

    input  wire [17:0]  cfg_fft_size,
    output wire          cfg_fft_size_invalid,

    input  wire [31:0]  s_axis_tdata,
    input  wire         s_axis_tvalid,
    output wire         s_axis_tready,
    input  wire         s_axis_tlast,

    output wire [31:0]  m_axis_tdata,
    output wire         m_axis_tvalid,
    input  wire         m_axis_tready,
    output wire         m_axis_tlast
);

    fft_engine dut (
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
        .m_axis_tlast         (m_axis_tlast)
    );

endmodule
