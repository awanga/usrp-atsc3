// bootstrap_detector_bare.v — Trivial single-instance wrapper, used only to
// seed the flatten+expose step in bootstrap_detector.sby's [script] (see
// cordic_bare.v and hdl/docs/formal_conventions.md for why white-box
// checks use `expose` rather than `bind` or a hierarchical reference).
//
// Fixes the small formal parameterization here, since `flatten` removes
// the wrapped module's parameters: L = 4 (HALF_SYMBOL_LOG2 = 2) and a
// 4-entry history RAM (MAX_WIN_LOG2 = 2). Index/window properties are
// width-generic, so they carry over to the real L = 2048 / 1024 sizes.
//
// Verification-only. Not synthesized, not part of rtl/.

`include "status_words.vh"

module bootstrap_detector_bare (
    input  wire                                   clk,
    input  wire                                   rst,
    input  wire [31:0]                            cfg_sample_rate_hz,
    input  wire signed [15:0]                     cfg_threshold_q15,
    input  wire [31:0]                            cfg_averaging_window,
    output wire                                   cfg_window_clamped,
    input  wire [31:0]                            s_axis_tdata,
    input  wire                                   s_axis_tvalid,
    output wire                                   s_axis_tready,
    input  wire                                   s_axis_tlast,
    output wire [`BOOTSTRAP_DETECTION_WIDTH-1:0]  m_axis_tdata,
    output wire                                   m_axis_tvalid,
    input  wire                                   m_axis_tready,
    output wire                                   m_axis_tlast,
    output wire                                   mon_valid,
    output wire [31:0]                            mon_metric,
    output wire signed [31:0]                     mon_cfo_hz
);

    bootstrap_detector #(
        .HALF_SYMBOL_LOG2 (2),
        .MAX_WIN_LOG2     (2)
    ) dut (
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
        .mon_cfo_hz           (mon_cfo_hz)
    );

endmodule
