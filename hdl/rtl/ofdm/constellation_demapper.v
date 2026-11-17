// SPDX-License-Identifier: CERN-OHL-W-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// constellation_demapper.v — soft demapper, uniform and non-uniform QAM
//
// Bit-exact port of lib/ofdm/constellation_demapper.cc demap_symbol()
// (ATSC3_FIXED_POINT=ON), one LLR beat per bit, MSB of the cell's bit label
// first.
//
// Uniform QPSK .. 4096-QAM (modulation 0..5): the Gray-code boundary slicer
// (demap_uniform_fixed / axis_bit_distance). Per axis, bit 0 is the
// coordinate's sign (distance = -x); bit b >= 1 is the distance from |x| to
// the nearest decision boundary of that bit, signed by the bit of the cell
// |x| falls in. The cell is floor(|x| / cw) clamped to half_side - 1 with
// cw = 2 * norm_const, counted by comparing against k * cw; cell c's bit is
// bit (bpa - 1 - b) of gray(half_side + c); a boundary k * cw lies between
// cells k-1 and k when that bit differs. LLR = (dist * scale + 2^30) >> 31,
// clamped to +/-clip. QPSK (0) slices (-Im) + j(-Re), negation saturated,
// so its labels follow A/322 Table C.1.1 (y0 from Im, y1 from Re).
//
// Non-uniform (6..11) use the model's max-log LLR,
//   LLR = ((min d^2 over points with the bit set) -
//          (min d^2 over points with it clear)) * scale, rounded >> 46,
// with d^2 the squared distance in Q1.15 units:
//   NUC_QPSK (6): the four A/322 Table C.1.1 points, no ROM.
//   NUC_16/64/256 (7..9): 2D NUC, the first-quadrant points from nuc2d.hex
//     (generated from the model by hdl/sim/golden/demapper_rom_gen) with the
//     model's quadrant expansion; all points scanned.
//   NUC_1024/4096 (10, 11): 1D NUC, a Cartesian product of two identical axis
//     level sets (nuc1d.hex). The set and clear minima of a bit on one axis
//     both include the same minimum over the other axis, so it cancels in
//     the difference: only the axis' own levels (32 / 64) are scanned, per
//     axis, which is bit-exact with scanning all 1024 / 4096 points.
//
// Not ported: nothing (every demap_symbol() path is covered).
//
// Interface contract:
//   - Input: one cell ci16 {re, im} per beat (equalizer output), TLAST
//     carried to the cell's last LLR beat. s_axis_tready only while idle.
//   - Output: bits_per_symbol int8 LLR beats per cell (2 .. 12), each held
//     until accepted; TLAST on the last LLR of a TLAST cell.
//   - Latency: roughly half_side * bits_per_axis (uniform), 2 * points
//     (2D NUC) or 2 * levels (1D NUC) cycles per cell; throughput is not yet
//     budgeted.
//   - Configuration (latched at reset, not read live): cfg_modulation 0..11
//     (the config::Modulation values) else QPSK with cfg_invalid;
//     cfg_code_rate 0..11 else 0 with cfg_invalid; cfg_llr_clip 1..127 else
//     127 with cfg_invalid; cfg_llr_scale_q16 below 2^31 (a value with bit
//     31 set is clamped, cfg_invalid); cfg_norm_const_q15 is the uniform
//     step ConstellationDemapper::norm_const_q15() (0 means cw = 0: cell 0,
//     as the model). Software writes the scale and step the model derives.
//   - Reset (synchronous, active-high) clears all state and the flag.
//
// AXI4-S: TDATA=ci16 TVALID TREADY TLAST in; TDATA=int8(LLR) TVALID TREADY
//         TLAST out.

module constellation_demapper #(
    parameter NUC2D_HEX = "nuc2d.hex",
    parameter NUC1D_HEX = "nuc1d.hex"
) (
    input  wire        clk,
    input  wire        rst,  // synchronous, active-high

    input  wire [3:0]  cfg_modulation,
    input  wire [3:0]  cfg_code_rate,
    input  wire [31:0] cfg_llr_scale_q16,
    input  wire [15:0] cfg_norm_const_q15,
    input  wire [7:0]  cfg_llr_clip,
    output reg         cfg_invalid,

    input  wire [31:0] s_axis_tdata,
    input  wire        s_axis_tvalid,
    output reg         s_axis_tready,
    input  wire        s_axis_tlast,

    output reg  [7:0]  m_axis_tdata,
    output reg         m_axis_tvalid,
    input  wire        m_axis_tready,
    output reg         m_axis_tlast
);

    localparam [4:0] S_IDLE  = 5'd0;
    localparam [4:0] S_U_A   = 5'd1;   // uniform: axis start, bit 0
    localparam [4:0] S_U_CNT = 5'd2;   // uniform: count the cell
    localparam [4:0] S_U_B   = 5'd3;   // uniform: bit init
    localparam [4:0] S_U_BL  = 5'd4;   // uniform: boundary loop
    localparam [4:0] S_U_BE  = 5'd5;   // uniform: bit result
    localparam [4:0] S_N2_I  = 5'd6;
    localparam [4:0] S_N2_A  = 5'd7;
    localparam [4:0] S_N2_W  = 5'd8;
    localparam [4:0] S_N2_U  = 5'd9;
    localparam [4:0] S_N2_C  = 5'd10;
    localparam [4:0] S_N1_I  = 5'd11;
    localparam [4:0] S_N1_A  = 5'd12;
    localparam [4:0] S_N1_W  = 5'd13;
    localparam [4:0] S_N1_U  = 5'd14;
    localparam [4:0] S_N1_C  = 5'd15;
    localparam [4:0] S_FLUSH = 5'd16;
    localparam [4:0] S_OUT   = 5'd17;

    localparam [15:0] QPSK_Q15 = 16'd23170;  // float_to_q15(1/sqrt(2)): the model's QPSK point

    function [15:0] sat_neg;
        input [15:0] v;
        sat_neg = (v == 16'h8000) ? 16'h7FFF : 16'd0 - v;
    endfunction

    reg [4:0] state;

    //--------------------------------------------------------------------
    // Configuration, validated now and latched during reset
    //--------------------------------------------------------------------

    wire mod_ok  = (cfg_modulation <= 4'd11);
    wire rate_ok = (cfg_code_rate <= 4'd11);
    wire clip_ok = (cfg_llr_clip != 8'd0) && !cfg_llr_clip[7];

    reg [3:0]  mod_r, rate_r;
    reg [30:0] scale_r;
    reg [15:0] norm_r;
    reg [6:0]  clip_r;

    wire is_uni      = (mod_r <= 4'd5);
    wire is_2d       = (mod_r >= 4'd6) && (mod_r <= 4'd9);  // NUC_QPSK uses the 2D engine
    wire is_qpsk_nuc = (mod_r == 4'd6);
    wire is_qpsk_uni = (mod_r == 4'd0);
    wire is_4096     = (mod_r == 4'd11);

    reg [3:0] bps;  // bits per symbol
    always @(*) begin
        case (mod_r)
            4'd0, 4'd6:  bps = 4'd2;
            4'd1, 4'd7:  bps = 4'd4;
            4'd2, 4'd8:  bps = 4'd6;
            4'd3, 4'd9:  bps = 4'd8;
            4'd4, 4'd10: bps = 4'd10;
            default:     bps = 4'd12;
        endcase
    end
    wire [2:0] bpa  = bps[3:1];                // bits per axis (1 .. 6)
    wire [5:0] half = 6'd1 << (bpa - 3'd1);    // half_side (1 .. 32)

    //--------------------------------------------------------------------
    // ROMs (registered read)
    //--------------------------------------------------------------------

    reg [31:0] rom2d [0:1007];
    reg [15:0] rom1d [0:1151];
    initial begin
        $readmemh(NUC2D_HEX, rom2d);
        $readmemh(NUC1D_HEX, rom1d);
    end

    reg [9:0]  rom2d_addr;
    reg [10:0] rom1d_addr;
    reg [31:0] rom2d_q;
    reg [15:0] rom1d_q;
    always @(posedge clk) begin
        rom2d_q <= rom2d[rom2d_addr];
        rom1d_q <= rom1d[rom1d_addr];
    end

    //--------------------------------------------------------------------
    // Cell, LLR memory and the shared converter
    //--------------------------------------------------------------------

    reg signed [15:0] x_re, x_im;
    reg               last_r;
    reg               axis;  // 0 = I, 1 = Q
    reg [7:0]         llr [0:11];
    reg [3:0]         out_idx;
    reg               wr_pend;
    reg [3:0]         wr_idx;

    // (a * scale + 2^(sh-1)) >>> sh, clamped to +/-clip; written to llr[wr_idx]
    // the cycle after conv_a is set
    reg signed [39:0] conv_a;
    reg        [5:0]  conv_sh;
    wire signed [71:0] conv_prod   = conv_a * $signed({1'b0, scale_r});
    wire signed [71:0] conv_biased = conv_prod + (72'sd1 <<< (conv_sh - 6'd1));
    wire signed [71:0] conv_shift  = conv_biased >>> conv_sh;
    wire signed [71:0] clip_pos    = {65'd0, clip_r};
    wire signed [71:0] clip_neg    = -clip_pos;
    wire        [7:0]  conv_out    = (conv_shift > clip_pos) ? {1'b0, clip_r} :
                                     (conv_shift < clip_neg) ? (~{1'b0, clip_r} + 8'd1) :
                                     conv_shift[7:0];

    wire signed [15:0] xa = axis ? x_im : x_re;

    //--------------------------------------------------------------------
    // Uniform slicer
    //--------------------------------------------------------------------

    wire [31:0] abs_x = xa[15] ? (~{{16{xa[15]}}, xa} + 32'd1) : {{16{xa[15]}}, xa};
    wire [31:0] cw    = {15'd0, norm_r, 1'b0};  // 2 * norm_const
    reg  [5:0]  k;
    reg  [31:0] bnd;     // k * cw
    reg  [5:0]  cell_c;
    reg  [2:0]  ubit;    // axis bit being resolved (1 .. bpa-1)
    reg  [31:0] best;
    reg         found;

    wire [5:0]  gi1  = half + k - 6'd1;
    wire [5:0]  gi2  = half + k;
    wire [5:0]  gr1  = gi1 ^ (gi1 >> 1);
    wire [5:0]  gr2  = gi2 ^ (gi2 >> 1);
    wire [2:0]  gpos = bpa - ubit - 3'd1;  // bit (bpa - 1 - b) of the Gray label
    wire        differ = gr1[gpos] != gr2[gpos];
    wire [31:0] bdist  = (abs_x >= bnd) ? (abs_x - bnd) : (bnd - abs_x);
    wire [5:0]  gc  = half + cell_c;
    wire [5:0]  grc = gc ^ (gc >> 1);
    wire        bit_here = grc[gpos];
    wire [31:0] u_mag  = found ? best : abs_x;
    wire signed [32:0] u_dist = bit_here ? -$signed({1'b0, u_mag}) : $signed({1'b0, u_mag});

    //--------------------------------------------------------------------
    // Max-log (2D and 1D NUC)
    //--------------------------------------------------------------------

    reg [8:0]  idx;
    reg [8*36-1:0] mn0;  // eight 36-bit minima, bit b at [36*b +: 36]
    reg [8*36-1:0] mn1;
    reg [3:0]  cb;

    wire [8:0] m_total = (mod_r == 4'd6) ? 9'd4 : (mod_r == 4'd7) ? 9'd16 :
                         (mod_r == 4'd8) ? 9'd64 : 9'd256;
    wire [2:0] nl      = (mod_r == 4'd7) ? 3'd2 : (mod_r == 4'd8) ? 3'd4 : 3'd6;
    wire [8:0] n_base  = 9'd1 << nl;
    wire [8:0] quad_w  = idx >> nl;
    wire [1:0] quad    = quad_w[1:0];
    wire [8:0] base_i  = idx & (n_base - 9'd1);
    wire [9:0] rate10  = {6'd0, rate_r};
    wire [9:0] addr2d  = (mod_r == 4'd7) ? ((rate10 << 2) + {1'b0, base_i}) :
                         (mod_r == 4'd8) ? (10'd48 + (rate10 << 4) + {1'b0, base_i}) :
                                           (10'd240 + (rate10 << 6) + {1'b0, base_i});
    wire signed [15:0] rom_re = rom2d_q[31:16];
    wire signed [15:0] rom_im = rom2d_q[15:0];
    wire signed [15:0] qp     = $signed(QPSK_Q15);
    // NUC_QPSK, Table C.1.1: label (y0, y1) = idx[1:0], Im < 0 for y0, Re < 0 for y1.
    wire signed [15:0] pt_re  = is_qpsk_nuc ? (idx[0] ? -qp : qp) : (quad[0] ? -rom_re : rom_re);
    wire signed [15:0] pt_im  = is_qpsk_nuc ? (idx[1] ? -qp : qp) : (quad[1] ? -rom_im : rom_im);
    wire signed [16:0] d_re   = {x_re[15], x_re} - {pt_re[15], pt_re};
    wire signed [16:0] d_im   = {x_im[15], x_im} - {pt_im[15], pt_im};
    wire        [33:0] dist2d = d_re * d_re + d_im * d_im;

    wire [10:0] rate11  = {7'd0, rate_r};
    wire [10:0] addr1d  = is_4096 ? (11'd384 + (rate11 << 6) + {4'd0, idx[6:0]})
                                  : ((rate11 << 5) + {4'd0, idx[6:0]});
    wire        lvl_neg = is_4096 ? idx[5] : idx[4];
    wire signed [15:0] rom_lvl = rom1d_q;
    wire signed [15:0] lvl     = lvl_neg ? -rom_lvl : rom_lvl;
    wire signed [16:0] d_ax    = {xa[15], xa} - {lvl[15], lvl};
    wire        [33:0] dist1d  = d_ax * d_ax;
    wire [2:0]  bpd     = is_4096 ? 3'd6 : 3'd5;
    wire [6:0]  last1d  = is_4096 ? 7'd63 : 7'd31;
    wire [3:0]  c1d_bit = bps - 4'd1 - {cb[2:0], 1'b0} - {3'd0, axis};  // symbol bit of axis bit cb

    wire signed [36:0] mdiff = $signed({1'b0, mn1[36*cb[2:0] +: 36]}) - $signed({1'b0, mn0[36*cb[2:0] +: 36]});

    integer b;

    always @(*) s_axis_tready = (state == S_IDLE);

    always @(posedge clk) begin
        if (rst) begin
            state         <= S_IDLE;
            mod_r         <= mod_ok ? cfg_modulation : 4'd0;
            rate_r        <= rate_ok ? cfg_code_rate : 4'd0;
            scale_r       <= cfg_llr_scale_q16[31] ? 31'h7FFFFFFF : cfg_llr_scale_q16[30:0];
            norm_r        <= cfg_norm_const_q15;
            clip_r        <= clip_ok ? cfg_llr_clip[6:0] : 7'd127;
            cfg_invalid   <= !mod_ok || !rate_ok || !clip_ok || cfg_llr_scale_q16[31];
            m_axis_tdata  <= 8'd0;
            m_axis_tvalid <= 1'b0;
            m_axis_tlast  <= 1'b0;
            x_re          <= 16'sd0;
            x_im          <= 16'sd0;
            last_r        <= 1'b0;
            axis          <= 1'b0;
            out_idx       <= 4'd0;
            wr_pend       <= 1'b0;
            wr_idx        <= 4'd0;
            conv_a        <= 40'sd0;
            conv_sh       <= 6'd31;
            k             <= 6'd0;
            bnd           <= 32'd0;
            cell_c        <= 6'd0;
            ubit          <= 3'd0;
            best          <= 32'd0;
            found         <= 1'b0;
            idx           <= 9'd0;
            cb            <= 4'd0;
            rom2d_addr    <= 10'd0;
            rom1d_addr    <= 11'd0;
        end else begin
            if (wr_pend) begin
                llr[wr_idx] <= conv_out;
                wr_pend     <= 1'b0;
            end

            case (state)
                S_IDLE: begin
                    if (s_axis_tvalid) begin
                        // Uniform QPSK slices (-Im) + j(-Re) (the model's
                        // demap_uniform_fixed), putting the A/322 Table C.1.1
                        // labels on the Gray grid; negation saturates.
                        x_re   <= is_qpsk_uni ? sat_neg(s_axis_tdata[15:0])
                                              : s_axis_tdata[31:16];
                        x_im   <= is_qpsk_uni ? sat_neg(s_axis_tdata[31:16])
                                              : s_axis_tdata[15:0];
                        last_r <= s_axis_tlast;
                        axis   <= 1'b0;
                        state  <= is_uni ? S_U_A : is_2d ? S_N2_I : S_N1_I;
                    end
                end

                //--------------------------------------------------------
                // Uniform boundary slicer
                //--------------------------------------------------------
                S_U_A: begin
                    // bit 0 of the axis: distance = -x
                    conv_a  <= -{{24{xa[15]}}, xa};
                    conv_sh <= 6'd31;
                    wr_idx  <= axis ? {1'b0, bpa} : 4'd0;
                    wr_pend <= 1'b1;
                    k       <= 6'd1;
                    bnd     <= cw;
                    cell_c  <= 6'd0;
                    ubit    <= 3'd1;
                    if (bpa == 3'd1) begin  // QPSK: sign bit only
                        if (!axis) begin
                            axis  <= 1'b1;
                        end else begin
                            state <= S_FLUSH;
                        end
                    end else begin
                        state <= S_U_CNT;
                    end
                end
                S_U_CNT: begin
                    if (k == half) begin
                        state <= S_U_B;
                    end else begin
                        if (cw != 32'd0 && abs_x >= bnd)
                            cell_c <= cell_c + 6'd1;
                        bnd <= bnd + cw;
                        k   <= k + 6'd1;
                    end
                end
                S_U_B: begin
                    found <= 1'b0;
                    best  <= 32'd0;
                    k     <= 6'd1;
                    bnd   <= cw;
                    state <= S_U_BL;
                end
                S_U_BL: begin
                    if (k == half) begin
                        state <= S_U_BE;
                    end else begin
                        if (differ && (!found || bdist < best)) begin
                            best <= bdist;
                        end
                        if (differ)
                            found <= 1'b1;
                        bnd <= bnd + cw;
                        k   <= k + 6'd1;
                    end
                end
                S_U_BE: begin
                    conv_a  <= {{7{u_dist[32]}}, u_dist};
                    conv_sh <= 6'd31;
                    wr_idx  <= axis ? ({1'b0, bpa} + {1'b0, ubit}) : {1'b0, ubit};
                    wr_pend <= 1'b1;
                    if ({1'b0, ubit} + 4'd1 < {1'b0, bpa}) begin
                        ubit  <= ubit + 3'd1;
                        state <= S_U_B;
                    end else if (!axis) begin
                        axis   <= 1'b1;
                        state  <= S_U_A;
                    end else begin
                        state <= S_FLUSH;
                    end
                end

                //--------------------------------------------------------
                // 2D NUC (and NUC_QPSK): scan every point
                //--------------------------------------------------------
                S_N2_I: begin
                    for (b = 0; b < 8; b = b + 1) begin
                        mn0[36*b +: 36] <= 36'hFFFFFFFFF;
                        mn1[36*b +: 36] <= 36'hFFFFFFFFF;
                    end
                    idx   <= 9'd0;
                    state <= S_N2_A;
                end
                S_N2_A: begin
                    rom2d_addr <= addr2d;
                    state      <= S_N2_W;
                end
                S_N2_W: state <= S_N2_U;
                S_N2_U: begin
                    for (b = 0; b < 8; b = b + 1) begin
                        if (b < bps) begin
                            if (idx[bps - 4'd1 - b[3:0]]) begin
                                if ({2'b00, dist2d} < mn1[36*b +: 36]) mn1[36*b +: 36] <= {2'b00, dist2d};
                            end else begin
                                if ({2'b00, dist2d} < mn0[36*b +: 36]) mn0[36*b +: 36] <= {2'b00, dist2d};
                            end
                        end
                    end
                    if (idx == m_total - 9'd1) begin
                        cb    <= 4'd0;
                        state <= S_N2_C;
                    end else begin
                        idx   <= idx + 9'd1;
                        state <= S_N2_A;
                    end
                end
                S_N2_C: begin
                    conv_a  <= {{3{mdiff[36]}}, mdiff};
                    conv_sh <= 6'd46;
                    wr_idx  <= cb;
                    wr_pend <= 1'b1;
                    cb      <= cb + 4'd1;
                    if (cb + 4'd1 == bps)
                        state <= S_FLUSH;
                end

                //--------------------------------------------------------
                // 1D NUC: scan the axis levels, I then Q
                //--------------------------------------------------------
                S_N1_I: begin
                    for (b = 0; b < 8; b = b + 1) begin
                        mn0[36*b +: 36] <= 36'hFFFFFFFFF;
                        mn1[36*b +: 36] <= 36'hFFFFFFFFF;
                    end
                    idx   <= 9'd0;
                    state <= S_N1_A;
                end
                S_N1_A: begin
                    rom1d_addr <= addr1d;
                    state      <= S_N1_W;
                end
                S_N1_W: state <= S_N1_U;
                S_N1_U: begin
                    for (b = 0; b < 6; b = b + 1) begin
                        if (b < bpd) begin
                            if (idx[b]) begin
                                if ({2'b00, dist1d} < mn1[36*b +: 36]) mn1[36*b +: 36] <= {2'b00, dist1d};
                            end else begin
                                if ({2'b00, dist1d} < mn0[36*b +: 36]) mn0[36*b +: 36] <= {2'b00, dist1d};
                            end
                        end
                    end
                    if (idx[6:0] == last1d) begin
                        cb    <= 4'd0;
                        state <= S_N1_C;
                    end else begin
                        idx   <= idx + 9'd1;
                        state <= S_N1_A;
                    end
                end
                S_N1_C: begin
                    conv_a  <= {{3{mdiff[36]}}, mdiff};
                    conv_sh <= 6'd46;
                    wr_idx  <= c1d_bit;
                    wr_pend <= 1'b1;
                    cb      <= cb + 4'd1;
                    if ({1'b0, cb[2:0]} + 4'd1 == {1'b0, bpd}) begin
                        if (!axis) begin
                            axis  <= 1'b1;
                            state <= S_N1_I;
                        end else begin
                            state <= S_FLUSH;
                        end
                    end
                end

                //--------------------------------------------------------
                // Serialize the cell's LLRs
                //--------------------------------------------------------
                S_FLUSH: begin
                    out_idx <= 4'd0;
                    state   <= S_OUT;
                end
                default: begin  // S_OUT
                    if (!m_axis_tvalid) begin
                        m_axis_tdata  <= llr[out_idx];
                        m_axis_tvalid <= 1'b1;
                        m_axis_tlast  <= last_r && (out_idx + 4'd1 == bps);
                    end else if (m_axis_tready) begin
                        m_axis_tvalid <= 1'b0;
                        if (out_idx + 4'd1 == bps) begin
                            state <= S_IDLE;
                        end else begin
                            out_idx <= out_idx + 4'd1;
                        end
                    end
                end
            endcase
        end
    end

    // Unused by design: high bits of the quadrant shift, 1D index bit 8
    wire unused_ok = &{1'b0, quad_w[8:2], idx[8], 1'b0};

endmodule
