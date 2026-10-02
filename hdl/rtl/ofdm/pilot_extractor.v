// pilot_extractor.v — scattered, continual and edge pilot extraction
//
// Bit-exact port of lib/ofdm/pilot_extractor.cc's PilotExtractor::extract()
// (ATSC3_FIXED_POINT=ON). The C++ builds three lists, sorts and dedups
// them; FFT bins arrive here in ascending carrier order, so the RTL
// classifies each bin as it streams past and emits one PilotSymbol per
// pilot carrier, already sorted and unique. A carrier that is several
// types is reported with the C++'s precedence: SCATTERED, then CONTINUAL,
// then EDGE.
//
// Per bin k (relative index rel = k - first_active):
//   scattered:  rel = offset + n * Dx*Dy, offset = (symbol mod Dy) * Dx.
//               A counter m = rel mod Dx*Dy runs across the active
//               window; the bin is scattered when m == offset (offset <
//               Dx*Dy, so this also implies rel >= offset).
//   continual:  rel equals the next entry of the continual-pilot ROM
//               (CP_ROM_HEX, generated from the C++ by
//               hdl/sim/golden/pilot_cp_rom_gen). Entries are ascending,
//               so one pointer walks the FFT size's region per symbol.
//   edge:       k == first_active or k == last_active.
// Reference value: +/-32767 + 0j from bit 0 of an x^11 + x^2 + 1 LFSR
// seeded 0x7FF at bin 0 and stepped once per bin, the C++'s
// pilot_sequence_[k].
//
// Interface contract:
//   - Input: one FFT bin per beat, fft_size beats per symbol, bin 0
//     first. s_axis_tuser carries the symbol index within the frame and is
//     sampled on bin 0 only; only symbol mod Dy matters (Dy divides 16).
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
//       cfg_pilot_pattern not 1..8  -> PP3 (the C++ default),
//                                      cfg_pattern_invalid
//       cfg_num_active_carriers 0   -> the FFT size's default (6913,
//                                      13825, 27649), as in the C++
//       cfg_num_active_carriers > fft_size -> the default,
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
    input  wire [3:0]  cfg_pilot_pattern,        // 1..8 = PP1..PP8
    input  wire [15:0] cfg_num_active_carriers,  // 0 = default for the size
    output reg         cfg_fft_size_invalid,
    output reg         cfg_pattern_invalid,
    output reg         cfg_num_active_invalid,
    output reg         framing_error,            // sticky

    `AXI4S_SLAVE(s_axis, `ATSC3_SAMPLE_WIDTH),
    input  wire [3:0]  s_axis_tuser,
    `AXI4S_MASTER(m_axis, `PILOT_SYMBOL_WIDTH)
);

    localparam ROM_DEPTH = 1024;
    localparam [15:0] Q15_ONE = 16'sd32767;
    localparam [15:0] Q15_MINUS_ONE = -16'sd32767;
    localparam [10:0] LFSR_SEED = 11'h7FF;

    //--------------------------------------------------------------------
    // Configuration, validated now and latched during reset
    //--------------------------------------------------------------------

    wire is_8k  = (cfg_fft_size == `ATSC3_FFT_8K);
    wire is_16k = (cfg_fft_size == `ATSC3_FFT_16K);
    wire is_32k = (cfg_fft_size == `ATSC3_FFT_32K);
    wire fft_size_valid_now = is_8k || is_16k || is_32k;

    reg [16:0] fft_now;
    reg [15:0] default_active_now;
    reg [9:0]  rom_base_now;
    always @(*) begin
        if (is_32k) begin
            fft_now            = 17'd32768;
            default_active_now = 16'd27649;
            rom_base_now       = 10'd512;
        end else if (is_16k) begin
            fft_now            = 17'd16384;
            default_active_now = 16'd13825;
            rom_base_now       = 10'd256;
        end else begin
            fft_now            = 17'd8192;
            default_active_now = 16'd6913;
            rom_base_now       = 10'd0;
        end
    end

    wire num_active_invalid_now = ({1'b0, cfg_num_active_carriers} > fft_now);
    wire [15:0] num_active_now =
        (cfg_num_active_carriers == 16'd0 || num_active_invalid_now) ? default_active_now :
                                                                       cfg_num_active_carriers;
    wire [16:0] first_now_wide = (fft_now - {1'b0, num_active_now}) >> 1;
    wire [16:0] last_now_wide  = first_now_wide + {1'b0, num_active_now} - 17'd1;

    // PP1..PP8: (Dx, Dy) = (3,4) (6,2) (6,4) (12,2) (12,4) (24,2) (24,4) (6,16)
    wire pattern_valid_now = (cfg_pilot_pattern >= 4'd1) && (cfg_pilot_pattern <= 4'd8);
    wire [3:0] pattern_now = pattern_valid_now ? cfg_pilot_pattern : 4'd3;
    reg [4:0] dx_now;
    reg [3:0] dy_mask_now;  // Dy - 1
    always @(*) begin
        case (pattern_now)
            4'd1:    begin dx_now = 5'd3;  dy_mask_now = 4'd3;  end
            4'd2:    begin dx_now = 5'd6;  dy_mask_now = 4'd1;  end
            4'd4:    begin dx_now = 5'd12; dy_mask_now = 4'd1;  end
            4'd5:    begin dx_now = 5'd12; dy_mask_now = 4'd3;  end
            4'd6:    begin dx_now = 5'd24; dy_mask_now = 4'd1;  end
            4'd7:    begin dx_now = 5'd24; dy_mask_now = 4'd3;  end
            4'd8:    begin dx_now = 5'd6;  dy_mask_now = 4'd15; end
            default: begin dx_now = 5'd6;  dy_mask_now = 4'd3;  end  // PP3
        endcase
    end
    wire [9:0] dy_now     = {6'd0, dy_mask_now} + 10'd1;
    wire [9:0] period_now = dx_now * dy_now;  // Dx*Dy, at most 96

    reg [14:0] last_bin_r;   // fft_size - 1
    reg [14:0] first_r;
    reg [14:0] last_r;
    reg [4:0]  dx_r;
    reg [3:0]  dy_mask_r;
    reg [6:0]  period_m1_r;  // Dx*Dy - 1
    reg [9:0]  rom_base_r;

    //--------------------------------------------------------------------
    // Per-bin classification
    //--------------------------------------------------------------------

    reg  [14:0] k;          // bin index within the symbol
    reg  [3:0]  sym_r;      // symbol index, latched on bin 0
    reg  [6:0]  m;          // (k - first_active) mod Dx*Dy, while active
    reg  [9:0]  cp_ptr;     // ROM address of the next continual pilot
    reg  [15:0] cp_next;    // cp_rom[cp_ptr]
    reg  [10:0] lfsr;

    wire        first_bin = (k == 15'd0);
    wire        last_bin  = (k == last_bin_r);
    wire [3:0]  sym       = first_bin ? s_axis_tuser : sym_r;
    wire [3:0]  sym_mod   = sym & dy_mask_r;            // symbol mod Dy
    wire [8:0]  offset    = {5'd0, sym_mod} * {4'd0, dx_r};  // < Dx*Dy

    wire        in_active = (k >= first_r) && (k <= last_r);
    wire [14:0] rel       = k - first_r;
    wire        is_sp     = in_active && ({2'b00, m} == offset);
    wire        is_cp     = in_active && (cp_next == {1'b0, rel});
    wire        is_edge   = (k == first_r) || (k == last_r);
    wire        is_pilot  = is_sp || is_cp || is_edge;
    wire [7:0]  pilot_type = is_sp ? `PILOT_TYPE_SCATTERED :
                             is_cp ? `PILOT_TYPE_CONTINUAL : `PILOT_TYPE_EDGE;

    wire fire_in = s_axis_tvalid && s_axis_tready;

    always @(*) begin
        s_axis_tready = !m_axis_tvalid || m_axis_tready;
    end

    //--------------------------------------------------------------------
    // Continual-pilot ROM: synchronous read at the pointer's next value,
    // so cp_next always equals cp_rom[cp_ptr].
    //--------------------------------------------------------------------

    reg [15:0] cp_rom [0:ROM_DEPTH-1];
`ifndef FORMAL_SKIP_ROM_INIT
    initial $readmemh(CP_ROM_HEX, cp_rom);
`endif

    reg [9:0] cp_ptr_next;
    always @(*) begin
        if (rst)
            cp_ptr_next = rom_base_now;
        else if (fire_in && last_bin)
            cp_ptr_next = rom_base_r;
        else if (fire_in && is_cp)
            cp_ptr_next = cp_ptr + 10'd1;
        else
            cp_ptr_next = cp_ptr;
    end

    always @(posedge clk) begin
        cp_ptr  <= cp_ptr_next;
        cp_next <= cp_rom[cp_ptr_next];
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
            dy_mask_r   <= dy_mask_now;
            period_m1_r <= period_now[6:0] - 7'd1;
            rom_base_r  <= rom_base_now;

            k     <= 15'd0;
            sym_r <= 4'd0;
            m     <= 7'd0;
            lfsr  <= LFSR_SEED;

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
                    lfsr <= LFSR_SEED;
                end else begin
                    k    <= k + 15'd1;
                    if (in_active)
                        m <= (m == period_m1_r) ? 7'd0 : m + 7'd1;
                    lfsr <= {lfsr[9:0], lfsr[10] ^ lfsr[1]};
                end
            end

            if (fire_in && is_pilot) begin
                m_axis_tvalid <= 1'b1;
                m_axis_tdata  <= {8'd0, pilot_type,
                                  s_axis_tdata[31:16], s_axis_tdata[15:0],
                                  lfsr[0] ? Q15_ONE : Q15_MINUS_ONE, 16'd0,
                                  1'b0, k};
                m_axis_tlast  <= (k == last_r);
            end else if (m_axis_tready) begin
                m_axis_tvalid <= 1'b0;
            end
        end
    end

    // Range-only bits: first_active < 16384, last_active < 32768 and
    // Dx*Dy <= 96 for every accepted configuration.
    wire unused_ok = &{1'b0, first_now_wide[16:15], last_now_wide[16:15], period_now[9:7], 1'b0};

endmodule
