// pilot_extractor.v — scattered, continual and edge pilot extraction
//
// Bit-exact port of lib/ofdm/pilot_extractor.cc's PilotExtractor::extract()
// (ATSC3_FIXED_POINT=ON), which follows ATSC A/322:2018 Section 8.1. The
// C++ builds three lists, sorts and dedups them; FFT bins arrive here in
// ascending carrier order, so the RTL classifies each bin as it streams
// past and emits one PilotSymbol per pilot carrier, already sorted and
// unique. A carrier that is several types is reported with the C++'s
// precedence: SCATTERED, then CONTINUAL, then EDGE.
//
// Per bin k (relative carrier rel = k - first_active, 0 .. NoC-1):
//   scattered:  rel mod Dx*Dy == Dx * (symbol mod Dy). A counter
//               m = rel mod Dx*Dy runs across the active window; the bin
//               is scattered when m == Dx * (symbol mod Dy).
//   continual:  common set: CP32 (Table D.1.1) is held in a ROM
//               (CP_ROM_HEX, generated from the C++ by
//               hdl/sim/golden/pilot_cp_rom_gen). The 16K / 8K sets are
//               every 2nd / 4th entry halved / quartered (Section
//               8.1.4.1; exact, CP32 is a multiple of 4), so one pointer walks the ROM with a
//               stride per FFT size. An entry is an absolute carrier index;
//               with carrier reduction the active window starts at
//               absolute carrier `shift` = Cred_coeff * Cunit / 2, so the
//               pointer skips entries below `shift` while the bins before
//               the active window go by (one entry per bin, at most six, and
//               first_active is at least 639) and then matches
//               entry == rel + shift.
//               additional set (Tables D.1.4 / D.1.5): up to three
//               relative indices per {FFT size, pattern, Cred_coeff},
//               looked up at reset from pilot_additional_cp_lut.v.
//   edge:       rel == 0 or rel == NoC-1.
// Reference value: +32767 for r_k = 0 and -32767 for r_k = 1 (+ 0j), r_k
// from the 13-bit generator of Section 8.1.2 (G(x) = 1 + x^9 + x^10 +
// x^12 + x^13), reloaded at every symbol and stepped once per active
// carrier.
//
// Interface contract:
//   - Input: one FFT bin per beat, fft_size beats per symbol, bin 0
//     first. s_axis_tuser carries the symbol index within the frame and is
//     sampled on bin 0 only; only symbol mod Dy matters (Dy divides 4).
//     TLAST in is checked, not used: a TLAST that disagrees with the bin
//     count sets the sticky framing_error, and counting continues.
//   - Output: one PilotSymbol per pilot carrier (layout:
//     status_words.vh PILOT_SYMBOL_*), carrier index absolute. TLAST marks
//     the last_active carrier's record, which is the symbol's last pilot
//     (it is always an edge pilot).
//   - Latency: a pilot bin accepted on edge t is valid on m_axis from t+1
//     and held, unchanged, until accepted. s_axis_tready =
//     !m_axis_tvalid || m_axis_tready, so a non-pilot bin is consumed
//     whenever the output register is free and the stream runs at one
//     bin per cycle without backpressure.
//   - Reset (synchronous, active-high): clears the output and status,
//     restarts at bin 0, and validates and latches the configuration (it
//     is not read live, same as cp_removal.v):
//       cfg_fft_size not 8K/16K/32K -> 8K, cfg_fft_size_invalid
//       cfg_pilot_pattern (the L1D_scattered_pilot_pattern code, 0 = SP3_2
//         ... 15 = SP32_4) not allowed for the FFT size (Annex D has no
//         layout for it) -> SP6_2 (allowed at every size), cfg_pattern_invalid
//       cfg_num_active_carriers 0   -> NoCmax for the size (6913, 13825,
//                                      27649), as in the C++
//       cfg_num_active_carriers not NoCmax - Cred_coeff * Cunit with
//         Cred_coeff 0..4 (Cunit 96 / 192 / 384) -> NoCmax,
//                                      cfg_num_active_invalid
//     first_active = (fft_size - num_active) >> 1 (hdl/docs/
//     wire_level_layouts.md), last_active = first_active + num_active - 1.
//
// AXI4-S: TDATA=ci16 TVALID TREADY TLAST TUSER(symbol index) in;
//         TDATA=PilotSymbol (96 bits) TVALID TREADY TLAST(symbol) out.

`include "axi4s_types.vh"
`include "status_words.vh"

module pilot_extractor #(
    parameter CP_ROM_HEX = "pilot_cp_positions.hex"
) (
    input  wire        clk,
    input  wire        rst,  // synchronous, active-high

    input  wire [17:0] cfg_fft_size,             // literal: 8192/16384/32768
    input  wire [3:0]  cfg_pilot_pattern,        // L1D_scattered_pilot_pattern code
    input  wire [15:0] cfg_num_active_carriers,  // 0 = default for the size
    output reg         cfg_fft_size_invalid,
    output reg         cfg_pattern_invalid,
    output reg         cfg_num_active_invalid,
    output reg         framing_error,            // sticky

    `AXI4S_SLAVE(s_axis, `ATSC3_SAMPLE_WIDTH),
    input  wire [3:0]  s_axis_tuser,
    `AXI4S_MASTER(m_axis, `PILOT_SYMBOL_WIDTH)
);

    localparam ROM_DEPTH = 256;
    localparam [15:0] Q15_ONE = 16'sd32767;
    localparam [15:0] Q15_MINUS_ONE = -16'sd32767;
    localparam [12:0] LFSR_INIT = 13'h001B;  // s[0..12] = 1101 1000 0000 0

    //--------------------------------------------------------------------
    // Configuration, validated now and latched during reset
    //--------------------------------------------------------------------

    wire is_8k  = (cfg_fft_size == `ATSC3_FFT_8K);
    wire is_16k = (cfg_fft_size == `ATSC3_FFT_16K);
    wire is_32k = (cfg_fft_size == `ATSC3_FFT_32K);
    wire fft_size_valid_now = is_8k || is_16k || is_32k;

    reg [16:0] fft_now;
    reg [15:0] noc_max_now;
    reg [8:0]  cunit_now;     // carrier reduction unit
    reg [1:0]  fft_sel_now;   // 0 = 8K, 1 = 16K, 2 = 32K
    reg [1:0]  stride_sh_now; // log2 of the CP32 entry stride
    always @(*) begin
        if (is_32k) begin
            fft_now       = 17'd32768;
            noc_max_now   = 16'd27649;
            cunit_now     = 9'd384;
            fft_sel_now   = 2'd2;
            stride_sh_now = 2'd0;
        end else if (is_16k) begin
            fft_now       = 17'd16384;
            noc_max_now   = 16'd13825;
            cunit_now     = 9'd192;
            fft_sel_now   = 2'd1;
            stride_sh_now = 2'd1;
        end else begin
            fft_now       = 17'd8192;
            noc_max_now   = 16'd6913;
            cunit_now     = 9'd96;
            fft_sel_now   = 2'd0;
            stride_sh_now = 2'd2;
        end
    end

    // NoC = NoCmax - Cred_coeff * Cunit (Table 7.1), Cred_coeff 0..4
    wire [15:0] diff_now  = noc_max_now - cfg_num_active_carriers;
    wire [15:0] cunit16   = {7'd0, cunit_now};
    wire        have_num  = (cfg_num_active_carriers != 16'd0);
    wire        le_max    = (cfg_num_active_carriers <= noc_max_now);
    reg  [2:0]  cred_match;
    reg         cred_ok;
    always @(*) begin
        cred_ok    = 1'b1;
        cred_match = 3'd0;
        if (diff_now == 16'd0)
            cred_match = 3'd0;
        else if (diff_now == cunit16)
            cred_match = 3'd1;
        else if (diff_now == {cunit16[14:0], 1'b0})
            cred_match = 3'd2;
        else if (diff_now == cunit16 + {cunit16[14:0], 1'b0})
            cred_match = 3'd3;
        else if (diff_now == {cunit16[13:0], 2'b00})
            cred_match = 3'd4;
        else
            cred_ok = 1'b0;
    end
    wire        num_active_invalid_now = have_num && !(le_max && cred_ok);
    wire [15:0] num_active_now = (have_num && !num_active_invalid_now) ?
                                 cfg_num_active_carriers : noc_max_now;
    wire [2:0]  cred_now       = (have_num && !num_active_invalid_now) ? cred_match : 3'd0;
    wire [16:0] first_now_wide = (fft_now - {1'b0, num_active_now}) >> 1;
    wire [16:0] last_now_wide  = first_now_wide + {1'b0, num_active_now} - 17'd1;

    // Absolute carrier of relative carrier 0: Cred_coeff * Cunit / 2
    wire [7:0] half_unit_now = cunit_now[8:1];
    reg  [9:0] shift_now;
    always @(*) begin
        case (cred_now)
            3'd0:    shift_now = 10'd0;
            3'd1:    shift_now = {2'd0, half_unit_now};
            3'd2:    shift_now = {1'b0, half_unit_now, 1'b0};
            3'd3:    shift_now = {2'd0, half_unit_now} + {1'b0, half_unit_now, 1'b0};
            default: shift_now = {half_unit_now, 2'b00};
        endcase
    end

    // Pattern: allowed for this FFT size (Annex D has a layout), else SP6_2
    wire        add_ok_a;
    wire [2:0]  add_en_a;
    wire [13:0] add0_a, add1_a, add2_a;
    wire        add_ok_b;
    wire [2:0]  add_en_b;
    wire [13:0] add0_b, add1_b, add2_b;
    pilot_additional_cp_lut lut_a (
        .key({fft_sel_now, cfg_pilot_pattern, cred_now}),
        .ok(add_ok_a), .en(add_en_a), .v0(add0_a), .v1(add1_a), .v2(add2_a)
    );
    pilot_additional_cp_lut lut_b (
        .key({fft_sel_now, 4'd4, cred_now}),
        .ok(add_ok_b), .en(add_en_b), .v0(add0_b), .v1(add1_b), .v2(add2_b)
    );
    wire        pattern_valid_now = add_ok_a;
    wire [3:0]  pattern_now = pattern_valid_now ? cfg_pilot_pattern : 4'd4;
    wire [2:0]  add_en_now  = pattern_valid_now ? add_en_a : add_en_b;
    wire [13:0] add0_now    = pattern_valid_now ? add0_a : add0_b;
    wire [13:0] add1_now    = pattern_valid_now ? add1_a : add1_b;
    wire [13:0] add2_now    = pattern_valid_now ? add2_a : add2_b;

    // (Dx, Dy) per Table 8.2
    reg [5:0] dx_now;
    reg       dy4_now;  // Dy == 4 (else 2)
    always @(*) begin
        dy4_now = pattern_now[0];
        case (pattern_now[3:1])
            3'd0:    dx_now = 6'd3;
            3'd1:    dx_now = 6'd4;
            3'd2:    dx_now = 6'd6;
            3'd3:    dx_now = 6'd8;
            3'd4:    dx_now = 6'd12;
            3'd5:    dx_now = 6'd16;
            3'd6:    dx_now = 6'd24;
            default: dx_now = 6'd32;
        endcase
    end
    wire [7:0] period_now = dy4_now ? {dx_now, 2'b00} : {1'b0, dx_now, 1'b0};  // Dx*Dy <= 128

    reg [14:0] last_bin_r;   // fft_size - 1
    reg [14:0] first_r;
    reg [14:0] last_r;
    reg [5:0]  dx_r;
    reg [3:0]  dy_mask_r;    // Dy - 1
    reg [6:0]  period_m1_r;  // Dx*Dy - 1
    reg [9:0]  shift_r;
    reg [1:0]  stride_sh_r;
    reg [2:0]  add_en_r;
    reg [13:0] add0_r, add1_r, add2_r;

    //--------------------------------------------------------------------
    // Per-bin classification
    //--------------------------------------------------------------------

    reg  [14:0] k;          // bin index within the symbol
    reg  [3:0]  sym_r;      // symbol index, latched on bin 0
    reg  [6:0]  m;          // (k - first_active) mod Dx*Dy, while active
    reg  [7:0]  cp_ptr;     // index of the next common continual pilot
    reg  [15:0] cp_raw;     // cp_rom[cp_ptr << stride]
    reg  [12:0] lfsr;

    wire        first_bin = (k == 15'd0);
    wire        last_bin  = (k == last_bin_r);
    wire [3:0]  sym       = first_bin ? s_axis_tuser : sym_r;
    wire [3:0]  sym_mod   = sym & dy_mask_r;                 // symbol mod Dy
    wire [8:0]  offset    = {5'd0, sym_mod} * {3'd0, dx_r};  // < Dx*Dy

    wire        started   = (k >= first_r);
    wire        in_active = started && (k <= last_r);
    wire [14:0] rel       = k - first_r;
    wire        is_sp     = in_active && ({2'b00, m} == offset);
    wire        is_edge   = (k == first_r) || (k == last_r);

    // Common CP entry on the FFT size's grid, against the absolute carrier
    // of the bin. Section 8.1.4.1 rounds up; every CP32 entry is a multiple
    // of 4, so the shift is exact.
    wire [15:0] cp_abs = cp_raw >> stride_sh_r;
    wire [15:0] cur    = started ? ({1'b0, rel} + {6'd0, shift_r}) : {6'd0, shift_r};
    wire        cp_hit = in_active && (cp_abs == cur);
    wire        cp_adv = (cp_abs < cur) || cp_hit;
    wire        add_hit = in_active &&
                          ((add_en_r[0] && ({1'b0, add0_r} == rel)) ||
                           (add_en_r[1] && ({1'b0, add1_r} == rel)) ||
                           (add_en_r[2] && ({1'b0, add2_r} == rel)));
    wire        is_cp     = cp_hit || add_hit;
    wire        is_pilot  = is_sp || is_cp || is_edge;
    wire [7:0]  pilot_type = is_sp ? `PILOT_TYPE_SCATTERED :
                             is_cp ? `PILOT_TYPE_CONTINUAL : `PILOT_TYPE_EDGE;

    wire fire_in = s_axis_tvalid && s_axis_tready;

    always @(*) begin
        s_axis_tready = !m_axis_tvalid || m_axis_tready;
    end

    //--------------------------------------------------------------------
    // Common continual-pilot ROM: synchronous read at the pointer's next
    // value, so cp_raw always equals cp_rom[cp_ptr << stride]. Entries
    // past the 192nd are FFFF, which never matches a carrier.
    //--------------------------------------------------------------------

    reg [15:0] cp_rom [0:ROM_DEPTH-1];
`ifndef FORMAL_SKIP_ROM_INIT
    initial $readmemh(CP_ROM_HEX, cp_rom);
`endif

    reg [7:0] cp_ptr_next;
    always @(*) begin
        if (rst)
            cp_ptr_next = 8'd0;
        else if (fire_in && last_bin)
            cp_ptr_next = 8'd0;
        else if (fire_in && cp_adv)
            cp_ptr_next = cp_ptr + 8'd1;
        else
            cp_ptr_next = cp_ptr;
    end

    wire [7:0] cp_addr_next = cp_ptr_next << stride_sh_r;

    always @(posedge clk) begin
        cp_ptr <= cp_ptr_next;
        cp_raw <= cp_rom[cp_addr_next];
    end

    //--------------------------------------------------------------------
    // Bin counter, pattern state and output register
    //--------------------------------------------------------------------

    always @(posedge clk) begin
        if (rst) begin
            cfg_fft_size_invalid   <= !fft_size_valid_now;
            cfg_pattern_invalid    <= !pattern_valid_now;
            cfg_num_active_invalid <= num_active_invalid_now;
            framing_error          <= 1'b0;

            last_bin_r  <= fft_now[14:0] - 15'd1;
            first_r     <= first_now_wide[14:0];
            last_r      <= last_now_wide[14:0];
            dx_r        <= dx_now;
            dy_mask_r   <= dy4_now ? 4'd3 : 4'd1;
            period_m1_r <= period_now[6:0] - 7'd1;
            shift_r     <= shift_now;
            stride_sh_r <= stride_sh_now;
            add_en_r    <= add_en_now;
            add0_r      <= add0_now;
            add1_r      <= add1_now;
            add2_r      <= add2_now;

            k     <= 15'd0;
            sym_r <= 4'd0;
            m     <= 7'd0;
            lfsr  <= LFSR_INIT;

            m_axis_tvalid <= 1'b0;
            m_axis_tdata  <= {`PILOT_SYMBOL_WIDTH{1'b0}};
            m_axis_tlast  <= 1'b0;
        end else begin
            if (fire_in) begin
                if (s_axis_tlast != last_bin)
                    framing_error <= 1'b1;
                if (first_bin)
                    sym_r <= s_axis_tuser;

                if (last_bin) begin
                    k    <= 15'd0;
                    m    <= 7'd0;
                    lfsr <= LFSR_INIT;
                end else begin
                    k    <= k + 15'd1;
                    if (in_active)
                        m <= (m == period_m1_r) ? 7'd0 : m + 7'd1;
                    if (started)
                        lfsr <= {lfsr[4] ^ lfsr[3] ^ lfsr[1] ^ lfsr[0], lfsr[12:1]};
                end
            end

            if (fire_in && is_pilot) begin
                m_axis_tvalid <= 1'b1;
                m_axis_tdata  <= {8'd0, pilot_type,
                                  s_axis_tdata[31:16], s_axis_tdata[15:0],
                                  lfsr[0] ? Q15_MINUS_ONE : Q15_ONE, 16'd0,
                                  1'b0, k};
                m_axis_tlast  <= (k == last_r);
            end else if (m_axis_tready) begin
                m_axis_tvalid <= 1'b0;
            end
        end
    end

    // Range-only bits: first_active < 16384, last_active < 32768,
    // Cred_coeff * Cunit / 2 <= 768 and Dx*Dy <= 128 for every accepted
    // configuration.
    wire unused_ok = &{1'b0, first_now_wide[16:15], last_now_wide[16:15], period_now[7],
                       add_ok_b, 1'b0};

endmodule
