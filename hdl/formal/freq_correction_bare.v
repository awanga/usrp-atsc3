// freq_correction_bare.v — single-instance wrapper that seeds the
// flatten+expose step in freq_correction.sby (see
// hdl/docs/formal_conventions.md).
//
// Verification-only. Not synthesized, not part of rtl/.

module freq_correction_bare (
    input  wire        clk,
    input  wire        rst,

    input  wire [31:0] cfg_phase_inc,
    input  wire        phase_reset,

    input  wire [31:0] s_axis_tdata,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,

    output wire [31:0] m_axis_tdata,
    output wire        m_axis_tvalid,
    input  wire        m_axis_tready,
    output wire        m_axis_tlast
);

    freq_correction dut (
        .clk           (clk),
        .rst           (rst),
        .cfg_phase_inc (cfg_phase_inc),
        .phase_reset   (phase_reset),
        .s_axis_tdata  (s_axis_tdata),
        .s_axis_tvalid (s_axis_tvalid),
        .s_axis_tready (s_axis_tready),
        .s_axis_tlast  (s_axis_tlast),
        .m_axis_tdata  (m_axis_tdata),
        .m_axis_tvalid (m_axis_tvalid),
        .m_axis_tready (m_axis_tready),
        .m_axis_tlast  (m_axis_tlast)
    );

endmodule
