// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// constellation_demapper_formal.v — Formal harness for
// hdl/rtl/ofdm/constellation_demapper.v
//
// Properties on ports plus a ghost copy of the configuration:
//   - LLR clamp bound: every output beat is within [-clip, +clip] (clip from
//     cfg_llr_clip when it is 1 .. 127, else 127), so never -128;
//   - framing: the beats of one cell number exactly bits_per_symbol, TLAST
//     only on the last of a cell accepted with TLAST, and the next cell is
//     accepted only after the last beat; s_axis_tready only while idle;
//   - output held (TVALID, data, TLAST) until accepted; nothing after reset;
//   - the configuration fallbacks: an out-of-range modulation behaves as
//     QPSK (two beats per cell) and sets cfg_invalid.
// The 72-bit LLR product is cut (cutpoint t:$mul): the clamp bound is a
// property of the clamp on whatever the product is, and the product itself
// is bit-exact in simulation against the C++ model with mutants.
//
// Scope, stated plainly: only the short paths are reachable within a bounded
// depth, so the modulation is constrained to QPSK (uniform, 2 beats) and
// NUC_QPSK (the 2D engine over its four points), plus the invalid-modulation
// fallback; the longer uniform slicer loops, the 2D/1D NUC scans and the ROM
// reads are simulation-only (every region, bit-exact). The ROM bound
// (|value| <= 32767) holds because float_to_q15 saturates symmetrically and
// demapper_rom_gen reproduces the model's points (it fails otherwise).
//
// Bounded: BMC depth 36 (the first cell through the 2D engine takes about 28 cycles, so
// one full cell and the start of the next), cover depth 36. Inputs, stalls and gaps are free.
//
// Run: hdl/formal/run_formal.sh constellation_demapper

module constellation_demapper_formal (
    input wire clk,
    input wire rst
);

    reg [3:0]  cfg_mod, cfg_rate;
    reg [31:0] cfg_scale;
    reg [15:0] cfg_norm;
    reg [7:0]  cfg_clip;
    reg [31:0] s_tdata;
    reg        s_tvalid, s_tlast, m_tready;
    wire       s_tready, m_tvalid, m_tlast, cfg_invalid;
    wire [7:0] m_tdata;

    constellation_demapper #(.NUC2D_HEX("nuc2d.hex"), .NUC1D_HEX("nuc1d.hex")) dut (
        .clk(clk), .rst(rst),
        .cfg_modulation(cfg_mod), .cfg_code_rate(cfg_rate),
        .cfg_llr_scale_q16(cfg_scale), .cfg_norm_const_q15(cfg_norm),
        .cfg_llr_clip(cfg_clip), .cfg_invalid(cfg_invalid),
        .s_axis_tdata(s_tdata), .s_axis_tvalid(s_tvalid),
        .s_axis_tready(s_tready), .s_axis_tlast(s_tlast),
        .m_axis_tdata(m_tdata), .m_axis_tvalid(m_tvalid),
        .m_axis_tready(m_tready), .m_axis_tlast(m_tlast)
    );

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;
    always @(*) if (!past_valid) assume (rst);
    always @(posedge clk)
        if (past_valid) assume ($stable(cfg_mod) && $stable(cfg_rate) && $stable(cfg_scale) &&
                                $stable(cfg_norm) && $stable(cfg_clip));
    // Reachable-depth scope (see the header)
    always @(*) assume (cfg_mod == 4'd0 || cfg_mod == 4'd6 || cfg_mod == 4'd12);

    // Ghost configuration
    wire        clip_ok = (cfg_clip != 8'd0) && !cfg_clip[7];
    wire [6:0]  clip_g  = clip_ok ? cfg_clip[6:0] : 7'd127;
    // every in-scope modulation has two bits per symbol
    localparam BPS = 2;

    reg [3:0] beats;      // output beats accepted for the current cell
    reg       cell_open;  // a cell has been accepted and not yet fully output
    reg       cell_last;  // its TLAST
    wire      accepted = s_tvalid && s_tready;
    wire      out_fire = m_tvalid && m_tready;

    always @(posedge clk) begin
        if (rst) begin
            beats     <= 4'd0;
            cell_open <= 1'b0;
        end else begin
            if (accepted) begin
                cell_open <= 1'b1;
                cell_last <= s_tlast;
                beats     <= 4'd0;
            end
            if (out_fire) begin
                beats <= beats + 4'd1;
                if (beats + 4'd1 == BPS) cell_open <= 1'b0;
            end
        end
    end

    reg        prev_stall, prev_last, prev_rst;
    reg [7:0]  prev_data;
    initial begin
        prev_stall = 1'b0;
        prev_rst   = 1'b0;
    end
    always @(posedge clk) begin
        prev_stall <= !rst && m_tvalid && !m_tready;
        prev_data  <= m_tdata;
        prev_last  <= m_tlast;
        prev_rst   <= rst;
    end

    wire signed [7:0] llr = m_tdata;

    always @(*) begin
        if (past_valid && !rst) begin
            if (m_tvalid) begin
                assert (cell_open);
                assert (beats < BPS);
                assert (llr <= $signed({1'b0, clip_g}));
                assert (llr >= -$signed({1'b0, clip_g}));
                assert (m_tlast == (cell_last && beats == BPS - 1));
            end
            if (cell_open) assert (!s_tready);
            if (prev_stall) assert (m_tvalid && m_tdata == prev_data && m_tlast == prev_last);
            assert (cfg_invalid == (cfg_mod > 4'd11 || cfg_rate > 4'd11 || !clip_ok || cfg_scale[31]));
        end
        if (past_valid && prev_rst) assert (!m_tvalid);
    end

    always @(posedge clk) begin
        if (past_valid && !rst) begin
            cover (out_fire && beats == 4'd1 && cell_last);
            cover (out_fire && cfg_mod == 4'd6 && llr != 8'sd0);
            cover (out_fire && cfg_mod == 4'd0 && llr < 0);
            cover (out_fire && cfg_mod == 4'd12);
        end
    end

endmodule
