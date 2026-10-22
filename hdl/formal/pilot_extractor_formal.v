// pilot_extractor_formal.v — Formal harness for pilot_extractor
//
// Main property (data integrity vs a ghost model): the cycle after every
// accepted bin, the output holds a record for it if and only if the
// ghost classifies the bin as a pilot, and that record has the bin's
// carrier index, its received sample unmodified, the ghost's type (with
// SCATTERED over CONTINUAL over EDGE), a +/-32767 + 0j reference, a zero
// reserved byte, and TLAST exactly on the last active carrier. With no
// accepted bin, a stalled record holds and otherwise TVALID drops; so
// no record is dropped, duplicated or invented. The ghost uses its own
// bin counter, configuration copy and classification: scattered by
// (rel mod Dx*Dy) == Dx * (symbol mod Dy) from the standard's formula (the
// RTL uses a running counter instead), edge by comparison, the common
// continual pilots by its own copy of the CP32 ROM at the RTL's pointer --
// made exact by the pointer invariants (the entry before the pointer is
// below the bin's absolute carrier, the entry at it is not) -- and the
// additional continual pilots by its own lookup of the generated table.
// The ghost also validates the configuration its own way (Cred_coeff
// found by trying 0..4), so the invalid-configuration fallbacks and the
// three flags are checked against it too.
//
// The reference's sign (the 13-bit generator) is not modelled:
// test_pilot_extractor.py checks every reference value against
// lib/ofdm/pilot_extractor.cc, which is checked against the standard's
// first 24 values and a bit-serial model of Figure 8.2.
//
// Assumptions: only that the first cycle is a reset. Configuration is
// latched at reset by both the DUT and the ghost, so cfg_* stay free; the
// output properties do not depend on the producer holding its beat.
//
// Bounds: prove is k-induction at depth 4 (smtbmc yices), closed by the
// invariants. Whole symbols (the framing check) are past any BMC depth,
// and so is a first pilot: the first active carrier of every legal
// configuration is at least 639. The cover task therefore starts from an
// arbitrary state that satisfies the invariants (assumed there, proved in
// the prove task) and reaches a record of each type within a few steps.
// test_pilot_extractor.py runs whole symbols at all sizes, patterns and
// reductions.
//
// Run: hdl/formal/run_formal.sh pilot_extractor

`include "axi4s_types.vh"
`include "status_words.vh"

module pilot_extractor_formal (
    input wire clk,
    input wire rst
);

`ifdef COVER
    localparam COVER_MODE = 1;
`else
    localparam COVER_MODE = 0;
`endif

    reg  [17:0] cfg_fft_size;
    reg  [3:0]  cfg_pilot_pattern;
    reg  [15:0] cfg_num_active_carriers;
    wire        cfg_fft_size_invalid, cfg_pattern_invalid, cfg_num_active_invalid;
    wire        framing_error;

    reg  [31:0] s_axis_tdata;
    reg         s_axis_tvalid;
    wire        s_axis_tready;
    reg         s_axis_tlast;
    reg  [3:0]  s_axis_tuser;

    wire [95:0] m_axis_tdata;
    wire        m_axis_tvalid;
    reg         m_axis_tready;
    wire        m_axis_tlast;

    wire [14:0] k_probe, first_probe, last_probe, last_bin_probe;
    wire [6:0]  m_probe;
    wire [7:0]  cp_ptr_probe;
    wire [15:0] cp_raw_probe;
    wire [3:0]  sym_probe;
    wire [5:0]  dx_probe;
    wire [3:0]  dy_mask_probe;
    wire [6:0]  period_m1_probe;
    wire [9:0]  shift_probe;
    wire [1:0]  stride_sh_probe;
    wire [2:0]  add_en_probe;
    wire [13:0] add0_probe, add1_probe, add2_probe;

    pilot_extractor_bare dut_top (
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
        .m_axis_tlast            (m_axis_tlast),
        .\dut.k           (k_probe),
        .\dut.m           (m_probe),
        .\dut.cp_ptr      (cp_ptr_probe),
        .\dut.cp_raw      (cp_raw_probe),
        .\dut.sym_r       (sym_probe),
        .\dut.first_r     (first_probe),
        .\dut.last_r      (last_probe),
        .\dut.last_bin_r  (last_bin_probe),
        .\dut.dx_r        (dx_probe),
        .\dut.dy_mask_r   (dy_mask_probe),
        .\dut.period_m1_r (period_m1_probe),
        .\dut.shift_r     (shift_probe),
        .\dut.stride_sh_r (stride_sh_probe),
        .\dut.add_en_r    (add_en_probe),
        .\dut.add0_r      (add0_probe),
        .\dut.add1_r      (add1_probe),
        .\dut.add2_r      (add2_probe)
    );

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;

    always @(*) begin
        if (!past_valid && !COVER_MODE)
            assume (rst);
    end

    //--------------------------------------------------------------------
    // Ghost configuration, validated and latched at reset like the DUT's
    //--------------------------------------------------------------------

    reg [15:0] g_rom [0:255];
    initial $readmemh("pilot_cp_positions.hex", g_rom);

    reg [16:0] c_fft, c_max;
    reg [8:0]  c_unit;
    reg [1:0]  c_sel;
    always @(*) begin
        case (cfg_fft_size)
            18'd16384: begin c_fft = 17'd16384; c_max = 17'd13825; c_unit = 9'd192; c_sel = 2'd1; end
            18'd32768: begin c_fft = 17'd32768; c_max = 17'd27649; c_unit = 9'd384; c_sel = 2'd2; end
            default:   begin c_fft = 17'd8192;  c_max = 17'd6913;  c_unit = 9'd96;  c_sel = 2'd0; end
        endcase
    end

    // Cred_coeff by trial: NoC = NoCmax - Cred_coeff * Cunit
    reg [2:0]  c_cred;
    reg        c_num_ok;
    integer    ci;
    always @(*) begin
        c_cred   = 3'd0;
        c_num_ok = (cfg_num_active_carriers == 16'd0);
        for (ci = 0; ci <= 4; ci = ci + 1)
            if ({1'b0, cfg_num_active_carriers} == c_max - ci * {8'd0, c_unit}) begin
                c_cred   = ci[2:0];
                c_num_ok = 1'b1;
            end
    end
    wire [16:0] c_num = (c_num_ok && cfg_num_active_carriers != 16'd0) ?
                        {1'b0, cfg_num_active_carriers} : c_max;
    wire [2:0]  c_cred_eff = (c_num_ok && cfg_num_active_carriers != 16'd0) ? c_cred : 3'd0;

    // Pattern: valid iff the generated table has an entry for it
    wire        c_pat_ok;
    wire [2:0]  c_pen;
    wire [13:0] c_pv0, c_pv1, c_pv2;
    pilot_additional_cp_lut c_lut (
        .key({c_sel, cfg_pilot_pattern, c_cred_eff}),
        .ok(c_pat_ok), .en(c_pen), .v0(c_pv0), .v1(c_pv1), .v2(c_pv2)
    );
    wire [3:0] c_pat = c_pat_ok ? cfg_pilot_pattern : 4'd4;

    reg [7:0] c_dx, c_dy;
    always @(*) begin
        case (c_pat)
            4'd0:  begin c_dx = 8'd3;  c_dy = 8'd2; end
            4'd1:  begin c_dx = 8'd3;  c_dy = 8'd4; end
            4'd2:  begin c_dx = 8'd4;  c_dy = 8'd2; end
            4'd3:  begin c_dx = 8'd4;  c_dy = 8'd4; end
            4'd4:  begin c_dx = 8'd6;  c_dy = 8'd2; end
            4'd5:  begin c_dx = 8'd6;  c_dy = 8'd4; end
            4'd6:  begin c_dx = 8'd8;  c_dy = 8'd2; end
            4'd7:  begin c_dx = 8'd8;  c_dy = 8'd4; end
            4'd8:  begin c_dx = 8'd12; c_dy = 8'd2; end
            4'd9:  begin c_dx = 8'd12; c_dy = 8'd4; end
            4'd10: begin c_dx = 8'd16; c_dy = 8'd2; end
            4'd11: begin c_dx = 8'd16; c_dy = 8'd4; end
            4'd12: begin c_dx = 8'd24; c_dy = 8'd2; end
            4'd13: begin c_dx = 8'd24; c_dy = 8'd4; end
            4'd14: begin c_dx = 8'd32; c_dy = 8'd2; end
            default: begin c_dx = 8'd32; c_dy = 8'd4; end
        endcase
    end

    wire [16:0] c_first = (c_fft - c_num) / 2;
    wire [16:0] c_last  = c_first + c_num - 17'd1;
    wire [16:0] c_shift = {14'd0, c_cred_eff} * {8'd0, c_unit[8:1]};

    reg [16:0] g_fft, g_first, g_last, g_shift, g_num;
    reg [1:0]  g_sel;
    reg [2:0]  g_cred;
    reg [3:0]  g_pat;
    reg [7:0]  g_dx, g_dy;
    reg        g_fft_bad, g_pat_bad, g_num_bad;
    always @(posedge clk) begin
        if (rst) begin
            g_fft     <= c_fft;
            g_num     <= c_num;
            g_first   <= c_first;
            g_last    <= c_last;
            g_shift   <= c_shift;
            g_sel     <= c_sel;
            g_cred    <= c_cred_eff;
            g_pat     <= c_pat;
            g_dx      <= c_dx;
            g_dy      <= c_dy;
            g_fft_bad <= (cfg_fft_size != 18'd8192 && cfg_fft_size != 18'd16384 &&
                          cfg_fft_size != 18'd32768);
            g_pat_bad <= !c_pat_ok;
            g_num_bad <= !c_num_ok;
        end
    end
    wire [15:0] g_period = g_dx * g_dy;

    // The ghost's own lookup of the latched configuration's additional pilots
    wire        g_lut_ok;
    wire [2:0]  g_en;
    wire [13:0] g_v0, g_v1, g_v2;
    pilot_additional_cp_lut g_lut (
        .key({g_sel, g_pat, g_cred}),
        .ok(g_lut_ok), .en(g_en), .v0(g_v0), .v1(g_v1), .v2(g_v2)
    );

    wire [16:0] g_stride = 17'd32768 / g_fft;  // 4, 2, 1
    function [16:0] ceil_div;
        input [15:0] value;
        input [16:0] stride;
        begin
            ceil_div = ({1'b0, value} + stride - 17'd1) / stride;
        end
    endfunction

    //--------------------------------------------------------------------
    // Ghost bin counter and classification of the bin now at the input
    //--------------------------------------------------------------------

    wire fire = s_axis_tvalid && s_axis_tready;

    reg [16:0] gk;
    reg [3:0]  g_sym;
    always @(posedge clk) begin
        if (rst) begin
            gk <= 17'd0;
        end else if (fire) begin
            gk <= (gk == g_fft - 17'd1) ? 17'd0 : gk + 17'd1;
            if (gk == 17'd0)
                g_sym <= s_axis_tuser;
        end
    end

    wire [3:0]  g_symv  = (gk == 17'd0) ? s_axis_tuser : g_sym;
    wire [16:0] g_off   = ({13'd0, g_symv} % {9'd0, g_dy}) * g_dx;
    wire        g_in    = (gk >= g_first) && (gk <= g_last);
    wire [16:0] g_rel   = gk - g_first;
    wire        g_sp    = g_in && ((g_rel % {1'b0, g_period}) == g_off);
    wire [16:0] g_cur   = (gk < g_first) ? g_shift : g_rel + g_shift;
    wire [15:0] g_entry = g_rom[cp_ptr_probe << stride_sh_probe];
    wire        g_cpc   = g_in && (ceil_div(g_entry, g_stride) == g_cur);
    wire        g_cpa   = g_in && ((g_en[0] && {3'd0, g_v0} == g_rel) ||
                                   (g_en[1] && {3'd0, g_v1} == g_rel) ||
                                   (g_en[2] && {3'd0, g_v2} == g_rel));
    wire        g_cp    = g_cpc || g_cpa;
    wire        g_edge  = (gk == g_first) || (gk == g_last);
    wire        g_pilot = g_sp || g_cp || g_edge;
    wire [7:0]  g_type  = g_sp ? `PILOT_TYPE_SCATTERED :
                          g_cp ? `PILOT_TYPE_CONTINUAL : `PILOT_TYPE_EDGE;

    reg        prev_rst, prev_fire, prev_pilot, prev_last;
    reg        prev_m_valid, prev_m_ready, prev_m_last;
    reg [95:0] prev_m_data;
    reg [7:0]  prev_type;
    reg [16:0] prev_gk;
    reg [31:0] prev_s_data;
    always @(posedge clk) begin
        prev_rst     <= rst;
        prev_fire    <= fire && !rst;
        prev_pilot   <= g_pilot;
        prev_type    <= g_type;
        prev_last    <= (gk == g_last);
        prev_gk      <= gk;
        prev_s_data  <= s_axis_tdata;
        prev_m_valid <= m_axis_tvalid;
        prev_m_ready <= m_axis_tready;
        prev_m_data  <= m_axis_tdata;
        prev_m_last  <= m_axis_tlast;
    end

    //--------------------------------------------------------------------
    // Main property: the output stream against the ghost
    //--------------------------------------------------------------------

    wire [15:0] ref_re = m_axis_tdata[`PILOT_SYMBOL_REFERENCE_RE_HI:`PILOT_SYMBOL_REFERENCE_RE_LO];

    always @(*) begin
        if (past_valid && !rst) begin
            assert (s_axis_tready == (!m_axis_tvalid || m_axis_tready));
        end
        if (past_valid && !prev_rst) begin
            if (prev_fire) begin
                assert (m_axis_tvalid == prev_pilot);
                if (prev_pilot) begin
                    assert (m_axis_tdata[`PILOT_SYMBOL_SUBCARRIER_INDEX_HI:`PILOT_SYMBOL_SUBCARRIER_INDEX_LO]
                            == prev_gk[15:0]);
                    assert (m_axis_tdata[`PILOT_SYMBOL_RECEIVED_RE_HI:`PILOT_SYMBOL_RECEIVED_IM_LO]
                            == prev_s_data);
                    assert (m_axis_tdata[`PILOT_SYMBOL_TYPE_HI:`PILOT_SYMBOL_TYPE_LO] == prev_type);
                    assert (m_axis_tdata[`PILOT_SYMBOL_REFERENCE_IM_HI:`PILOT_SYMBOL_REFERENCE_IM_LO]
                            == 16'd0);
                    assert (ref_re == 16'h7FFF || ref_re == 16'h8001);
                    assert (m_axis_tdata[95:88] == 8'd0);
                    assert (m_axis_tlast == prev_last);
                end
            end else if (prev_m_valid && !prev_m_ready) begin
                assert (m_axis_tvalid);
                assert (m_axis_tdata == prev_m_data);
                assert (m_axis_tlast == prev_m_last);
            end else begin
                assert (!m_axis_tvalid);
            end
        end
        if (past_valid && prev_rst) begin
            assert (!m_axis_tvalid);
            assert (cfg_fft_size_invalid == g_fft_bad);
            assert (cfg_pattern_invalid == g_pat_bad);
            assert (cfg_num_active_invalid == g_num_bad);
        end
    end

    //--------------------------------------------------------------------
    // Invariants tying the DUT's state to the ghost (they close the
    // induction, and the pointer group is the ROM bound). In the cover task
    // they are assumed, so the search starts from any state they allow.
    //--------------------------------------------------------------------

    wire [16:0] g_m_span = ((gk > g_last + 17'd1) ? g_last + 17'd1 : gk) - g_first;
    wire [16:0] g_entry_abs = ceil_div(g_entry, g_stride);
    wire [15:0] g_prev_entry = g_rom[(cp_ptr_probe - 8'd1) << stride_sh_probe];
    wire [16:0] g_prev_abs = ceil_div(g_prev_entry, g_stride);

    wire inv_enable = COVER_MODE ? !rst : (past_valid && !rst && !prev_rst);

    reg inv_ok;
    always @(*) begin
        inv_ok = 1'b1;
        // configuration copies agree with the ghost's
        inv_ok = inv_ok && (first_probe == g_first[14:0] && last_probe == g_last[14:0]);
        inv_ok = inv_ok && (last_bin_probe == g_fft[14:0] - 15'd1);
        inv_ok = inv_ok && ({2'd0, dx_probe} == g_dx[7:0] + 8'd0 && {4'd0, dy_mask_probe} == g_dy - 8'd1);
        inv_ok = inv_ok && ({9'd0, period_m1_probe} == g_period - 16'd1);
        inv_ok = inv_ok && ({7'd0, shift_probe} == g_shift);
        inv_ok = inv_ok && ({15'd0, stride_sh_probe} == (g_fft == 17'd8192 ? 17'd2 :
                                                          (g_fft == 17'd16384 ? 17'd1 : 17'd0)));
        inv_ok = inv_ok && (add_en_probe == g_en && add0_probe == g_v0 &&
                            add1_probe == g_v1 && add2_probe == g_v2);
        // ghost configuration is a legal one
        inv_ok = inv_ok && (g_fft == 17'd8192 || g_fft == 17'd16384 || g_fft == 17'd32768);
        inv_ok = inv_ok && g_lut_ok;
        inv_ok = inv_ok && (g_last < g_fft && g_first <= g_last && g_first >= 17'd639);
        inv_ok = inv_ok && (g_num == g_last - g_first + 17'd1);
        inv_ok = inv_ok && ((g_dx == 8'd3 || g_dx == 8'd4 || g_dx == 8'd6 || g_dx == 8'd8 ||
                             g_dx == 8'd12 || g_dx == 8'd16 || g_dx == 8'd24 || g_dx == 8'd32) &&
                            (g_dy == 8'd2 || g_dy == 8'd4));
        // bin counter and symbol index
        inv_ok = inv_ok && (gk < g_fft && {2'b00, k_probe} == gk);
        if (gk != 17'd0)
            inv_ok = inv_ok && (sym_probe == g_sym);
        // scattered counter
        if (gk <= g_first)
            inv_ok = inv_ok && (m_probe == 7'd0);
        else
            inv_ok = inv_ok && ({10'd0, m_probe} == g_m_span % {1'b0, g_period});
        // common continual pilot pointer: register tracks the ROM; before the
        // window the pointer has skipped the entries below the window's first
        // absolute carrier, one per bin; inside it, the entry before the
        // pointer is below the bin's absolute carrier and the entry at it is not
        inv_ok = inv_ok && (cp_raw_probe == g_entry);
        inv_ok = inv_ok && (cp_ptr_probe <= 8'd192 >> stride_sh_probe);
        if (gk <= g_first) begin
            inv_ok = inv_ok && ({9'd0, cp_ptr_probe} <= gk);
            if (cp_ptr_probe != 8'd0)
                inv_ok = inv_ok && (g_prev_abs < g_shift);
            inv_ok = inv_ok && (g_entry_abs >= g_shift || {9'd0, cp_ptr_probe} == gk);
            if (gk == g_first)
                inv_ok = inv_ok && (g_entry_abs >= g_shift);
        end else if (gk <= g_last + 17'd1) begin
            inv_ok = inv_ok && (g_entry_abs >= gk - g_first + g_shift);
            if (cp_ptr_probe != 8'd0)
                inv_ok = inv_ok && (g_prev_abs < gk - g_first + g_shift);
        end
    end

    always @(*) begin
        if (inv_enable) begin
            if (COVER_MODE)
                assume (inv_ok);
            else
                assert (inv_ok);
        end
    end

    //--------------------------------------------------------------------
    // Cover: a record of each type, an additional continual pilot, and a
    // scattered pilot on an edge
    //--------------------------------------------------------------------

    always @(*) begin
        if (past_valid && !rst && m_axis_tvalid) begin
            cover (m_axis_tdata[87:80] == `PILOT_TYPE_CONTINUAL);
            cover (m_axis_tdata[87:80] == `PILOT_TYPE_EDGE);
            cover (m_axis_tdata[87:80] == `PILOT_TYPE_SCATTERED &&
                   m_axis_tdata[15:0] == g_first[15:0]);
            cover (m_axis_tdata[87:80] == `PILOT_TYPE_SCATTERED &&
                   m_axis_tdata[15:0] != g_first[15:0]);
        end
    end

endmodule
