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
// (rel - offset) mod Dx*Dy == 0 with rel >= offset (the RTL uses a
// running counter instead), edge by comparison, and continual by its own
// copy of the ROM at the RTL's pointer -- made exact by the pointer
// invariants (the entry before the pointer is below the carrier's
// relative index, the entry at it is not, and the pointer stays between
// its region's base and end marker: the ROM bound).
//
// The reference's sign (the LFSR) is not modelled: test_pilot_extractor.py
// checks every reference value against lib/ofdm/pilot_extractor.cc.
//
// Assumptions: only that the first cycle is a reset. Configuration is
// latched at reset by both the DUT and the ghost, so cfg_* stay free; the
// output properties do not depend on the producer holding its beat.
//
// Bounds: prove is k-induction at depth 4 (smtbmc yices), closed by the
// invariants above. Cover reaches a record of each type at depth 56: with
// every carrier active the first continual pilot is carrier 44. A whole
// symbol (TLAST, the framing check) is past any BMC depth;
// test_pilot_extractor.py runs whole symbols at all sizes and patterns.
// The same depth limit means two committed mutants (continual beating
// scattered, the ROM pointer skipping an entry) fail only the induction
// here, with no counterexample: their first wrong record needs a
// continual pilot (relative carrier 44 or later), or a scattered one
// landing on it (over 100 bins). The cocotb bench kills both directly.
//
// Run: hdl/formal/run_formal.sh pilot_extractor

`include "axi4s_types.vh"
`include "status_words.vh"

module pilot_extractor_formal (
    input wire clk,
    input wire rst
);

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
    wire [9:0]  cp_ptr_probe, rom_base_probe;
    wire [15:0] cp_next_probe;
    wire [3:0]  sym_probe;
    wire [4:0]  dx_probe;
    wire [3:0]  dy_mask_probe;
    wire [6:0]  period_m1_probe;

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
        .\dut.k          (k_probe),
        .\dut.m          (m_probe),
        .\dut.cp_ptr     (cp_ptr_probe),
        .\dut.cp_next    (cp_next_probe),
        .\dut.sym_r      (sym_probe),
        .\dut.first_r    (first_probe),
        .\dut.last_r     (last_probe),
        .\dut.last_bin_r (last_bin_probe),
        .\dut.rom_base_r (rom_base_probe),
        .\dut.dx_r       (dx_probe),
        .\dut.dy_mask_r  (dy_mask_probe),
        .\dut.period_m1_r (period_m1_probe)
    );

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;

    always @(*) begin
        if (!past_valid)
            assume (rst);
    end

    //--------------------------------------------------------------------
    // Ghost configuration, latched at reset like the DUT's
    //--------------------------------------------------------------------

    reg [15:0] g_rom [0:1023];
    initial $readmemh("pilot_cp_positions.hex", g_rom);

    reg [16:0] c_fft, c_default;
    reg [9:0]  c_base, c_count;  // c_count: entries before the end marker
    always @(*) begin
        case (cfg_fft_size)
            18'd16384: begin c_fft = 17'd16384; c_default = 17'd13825;
                             c_base = 10'd256; c_count = 10'd183; end
            18'd32768: begin c_fft = 17'd32768; c_default = 17'd27649;
                             c_base = 10'd512; c_count = 10'd365; end
            default:   begin c_fft = 17'd8192;  c_default = 17'd6913;
                             c_base = 10'd0;   c_count = 10'd92;  end
        endcase
    end
    wire [16:0] c_num = (cfg_num_active_carriers == 16'd0 ||
                         {1'b0, cfg_num_active_carriers} > c_fft) ?
                        c_default : {1'b0, cfg_num_active_carriers};

    reg [7:0] c_dx, c_dy;
    always @(*) begin
        case (cfg_pilot_pattern)
            4'd1: begin c_dx = 8'd3;  c_dy = 8'd4;  end
            4'd2: begin c_dx = 8'd6;  c_dy = 8'd2;  end
            4'd4: begin c_dx = 8'd12; c_dy = 8'd2;  end
            4'd5: begin c_dx = 8'd12; c_dy = 8'd4;  end
            4'd6: begin c_dx = 8'd24; c_dy = 8'd2;  end
            4'd7: begin c_dx = 8'd24; c_dy = 8'd4;  end
            4'd8: begin c_dx = 8'd6;  c_dy = 8'd16; end
            default: begin c_dx = 8'd6; c_dy = 8'd4; end  // PP3, also for 0 and 9..15
        endcase
    end

    reg [16:0] g_fft, g_first, g_last;
    reg [9:0]  g_base, g_count;
    reg [7:0]  g_dx, g_dy;
    always @(posedge clk) begin
        if (rst) begin
            g_fft   <= c_fft;
            g_first <= (c_fft - c_num) / 2;
            g_last  <= (c_fft - c_num) / 2 + c_num - 17'd1;
            g_base  <= c_base;
            g_count <= c_count;
            g_dx    <= c_dx;
            g_dy    <= c_dy;
        end
    end
    wire [15:0] g_period = g_dx * g_dy;

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
    wire        g_sp    = g_in && (g_rel >= g_off) && ((g_rel - g_off) % {1'b0, g_period} == 0);
    wire        g_cp    = g_in && (g_rom[cp_ptr_probe] == g_rel[15:0]);
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
        end
    end

    //--------------------------------------------------------------------
    // Invariants tying the DUT's state to the ghost (they close the
    // induction, and the last group is the ROM bound)
    //--------------------------------------------------------------------

    wire [16:0] g_m_span = ((gk > g_last + 17'd1) ? g_last + 17'd1 : gk) - g_first;

    always @(*) begin
        if (past_valid && !rst && !prev_rst) begin
            assert (first_probe == g_first[14:0] && last_probe == g_last[14:0]);
            assert (last_bin_probe == g_fft[14:0] - 15'd1 && rom_base_probe == g_base);
            assert ((g_dx == 8'd3  && g_dy == 8'd4) || (g_dx == 8'd6  && g_dy == 8'd2) ||
                    (g_dx == 8'd6  && g_dy == 8'd4) || (g_dx == 8'd12 && g_dy == 8'd2) ||
                    (g_dx == 8'd12 && g_dy == 8'd4) || (g_dx == 8'd24 && g_dy == 8'd2) ||
                    (g_dx == 8'd24 && g_dy == 8'd4) || (g_dx == 8'd6  && g_dy == 8'd16));
            assert ((g_fft == 17'd8192  && g_base == 10'd0   && g_count == 10'd92) ||
                    (g_fft == 17'd16384 && g_base == 10'd256 && g_count == 10'd183) ||
                    (g_fft == 17'd32768 && g_base == 10'd512 && g_count == 10'd365));
            assert ({3'd0, dx_probe} == g_dx && {4'd0, dy_mask_probe} == g_dy - 8'd1);
            assert ({9'd0, period_m1_probe} == g_period - 16'd1);
            assert (g_fft == 17'd8192 || g_fft == 17'd16384 || g_fft == 17'd32768);
            assert (g_last < g_fft && g_first <= g_last);
            assert (gk < g_fft && {2'b00, k_probe} == gk);
            if (gk != 17'd0)
                assert (sym_probe == g_sym);

            if (gk <= g_first)
                assert (m_probe == 7'd0);
            else
                assert ({10'd0, m_probe} == g_m_span % {1'b0, g_period});

            assert (cp_next_probe == g_rom[cp_ptr_probe]);
            assert (cp_ptr_probe >= g_base && cp_ptr_probe <= g_base + g_count);
            if (gk <= g_first) begin
                assert (cp_ptr_probe == g_base);
            end else if (gk <= g_last + 17'd1) begin
                assert ({1'b0, g_rom[cp_ptr_probe]} >= gk - g_first);
                if (cp_ptr_probe != g_base)
                    assert ({1'b0, g_rom[cp_ptr_probe - 10'd1]} < gk - g_first);
            end
        end
    end

    //--------------------------------------------------------------------
    // Cover: a record of each type, and a scattered pilot on an edge
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
