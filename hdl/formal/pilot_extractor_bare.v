// pilot_extractor_bare.v — single-instance wrapper that seeds the
// flatten+expose step in pilot_extractor.sby (see
// hdl/docs/formal_conventions.md). No parameter override: the real
// continual-pilot ROM is loaded (the .sby copies it into the work
// directory), so the pointer invariants are checked against the actual
// positions.
//
// Verification-only. Not synthesized, not part of rtl/.

module pilot_extractor_bare (
    input  wire        clk,
    input  wire        rst,

    input  wire [17:0] cfg_fft_size,
    input  wire [3:0]  cfg_pilot_pattern,
    input  wire [15:0] cfg_num_active_carriers,
    output wire        cfg_fft_size_invalid,
    output wire        cfg_pattern_invalid,
    output wire        cfg_num_active_invalid,
    output wire        framing_error,

    input  wire [31:0] s_axis_tdata,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,
    input  wire [3:0]  s_axis_tuser,

    output wire [95:0] m_axis_tdata,
    output wire        m_axis_tvalid,
    input  wire        m_axis_tready,
    output wire        m_axis_tlast
);

    pilot_extractor dut (
        .clk                     (clk),
        .rst                     (rst),
        .cfg_fft_size            (cfg_fft_size),
        .cfg_pilot_pattern       (cfg_pilot_pattern),
        .cfg_num_active_carriers (cfg_num_active_carriers),
        .cfg_fft_size_invalid    (cfg_fft_size_invalid),
        .cfg_pattern_invalid     (cfg_pattern_invalid),
        .cfg_num_active_invalid  (cfg_num_active_invalid),
        .framing_error           (framing_error),
        .s_axis_tdata            (s_axis_tdata),
        .s_axis_tvalid           (s_axis_tvalid),
        .s_axis_tready           (s_axis_tready),
        .s_axis_tlast            (s_axis_tlast),
        .s_axis_tuser            (s_axis_tuser),
        .m_axis_tdata            (m_axis_tdata),
        .m_axis_tvalid           (m_axis_tvalid),
        .m_axis_tready           (m_axis_tready),
        .m_axis_tlast            (m_axis_tlast)
    );

endmodule
