// timing_recovery_bare.v — Trivial single-instance wrapper, used only to
// seed the flatten+expose step in timing_recovery.sby's [script] that
// promotes timing_recovery's internal state/mu_q16 registers to
// top-level ports so the formal harness can reference them (see
// hdl/docs/formal_conventions.md and axi4s_skid_buffer_bare.v for why
// this replaces a `bind`-based checker).
//
// No parameter override: this block's arithmetic (the polyphase FIR's
// 16x16 multiply-accumulate, the loop filter's 16x64-bit multiplies) is
// the same class of "too wide for smtbmc/z3 directly" datapath as
// bootstrap_detector's, so timing_recovery.sby cuts multipliers and uses
// ABC PDR via prove_pdr.sh, the same treatment -- see that file.
//
// Verification-only. Not synthesized, not part of rtl/.

module timing_recovery_bare (
    input  wire                clk,
    input  wire                rst,

    input  wire signed [15:0]  cfg_kp_q15,
    input  wire signed [15:0]  cfg_ki_q15,
    input  wire [7:0]          cfg_samples_per_symbol,
    input  wire signed [15:0]  cfg_initial_offset_q15,
    input  wire                cfg_locked,

    input  wire [31:0]         s_axis_tdata,
    input  wire                s_axis_tvalid,
    output wire                s_axis_tready,
    input  wire                s_axis_tlast,

    output wire [31:0]         m_axis_tdata,
    output wire                m_axis_tvalid,
    input  wire                m_axis_tready,
    output wire                m_axis_tlast,

    output wire                mon_valid,
    output wire [15:0]         mon_mu_q16,
    output wire signed [63:0]  mon_timing_error_q15
);

    timing_recovery dut (
        .clk                    (clk),
        .rst                    (rst),
        .cfg_kp_q15             (cfg_kp_q15),
        .cfg_ki_q15             (cfg_ki_q15),
        .cfg_samples_per_symbol (cfg_samples_per_symbol),
        .cfg_initial_offset_q15 (cfg_initial_offset_q15),
        .cfg_locked             (cfg_locked),
        .s_axis_tdata           (s_axis_tdata),
        .s_axis_tvalid          (s_axis_tvalid),
        .s_axis_tready          (s_axis_tready),
        .s_axis_tlast           (s_axis_tlast),
        .m_axis_tdata           (m_axis_tdata),
        .m_axis_tvalid          (m_axis_tvalid),
        .m_axis_tready          (m_axis_tready),
        .m_axis_tlast           (m_axis_tlast),
        .mon_valid              (mon_valid),
        .mon_mu_q16             (mon_mu_q16),
        .mon_timing_error_q15   (mon_timing_error_q15)
    );

endmodule
