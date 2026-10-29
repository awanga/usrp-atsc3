// SPDX-License-Identifier: CERN-OHL-W-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// frame_correlator.v — normalized preamble correlation magnitude
//
// Bit-exact port of FrameSync::correlate_preamble() in
// lib/sync/frame_sync.cc (ATSC3_FIXED_POINT=ON) for one offset: the
// magnitude of sum(sig[i] * conj(ref[i])) over len samples, divided by
// sqrt(sum |sig|^2) * sqrt(sum |ref|^2), as a Q1.15 ratio.
//
// Datapath, in the C++'s order:
//   1. One sample per cycle from the caller's RAM: four 16x16 products
//      into 64-bit corr_re / corr_im, and the sig and ref power sums.
//      ref[i] is the 13-entry period table frame_sync_ref.vh (the model's
//      placeholder sequence repeats every 13 samples).
//   2. sig_mag = isqrt(sig_power), ref_mag = isqrt(ref_power), one shared
//      isqrt_seq run twice; norm = sig_mag * ref_mag. Either power being
//      zero gives ratio 0.
//   3. corr_re / corr_im are shifted right (arithmetic, together) until both
//      fit +/-32767 (at most 48 shifts), then CORDIC vectoring gives the
//      magnitude, which is shifted back left by the same amount.
//   4. ratio = (corr_mag << 15) / norm, 64-bit unsigned floor division,
//      capped at 65535 (2.0).
//
// Interface: with busy low, drive base and len and pulse start (sampled on
// that edge; ignored while busy). The block reads len samples from
// rd_addr = base, base+1, ...; rd_data must be the registered read of the
// address presented the previous cycle (a synchronous-read RAM). busy is
// high from the start edge until done, which pulses for one cycle with
// ratio valid; ratio holds until the next done. Reset clears busy, done and
// ratio and abandons any correlation in flight.
//
// len is 4..32768 (16 bits); the accumulators are 64-bit like the C++'s
// int64_t, whose bound for len <= 32768 is below 2^47. Cycles per
// correlation: len + 2 + 2 * 25 (isqrt) + up to 48 (shift) + 16 (CORDIC) +
// 65 (divide) + a few control cycles, sequential by design.

`include "cordic_types.vh"

module frame_correlator #(
    parameter ADDR_W = 16
) (
    input  wire              clk,
    input  wire              rst,  // synchronous, active-high

    input  wire              start,
    input  wire [ADDR_W-1:0] base,
    input  wire [15:0]       len,

    output reg               busy,
    output reg               done,   // one-cycle pulse
    output reg  [15:0]       ratio,  // Q1.15, 0..65535

    output reg  [ADDR_W-1:0] rd_addr,
    input  wire [31:0]       rd_data
);

    localparam [3:0] C_IDLE   = 4'd0,
                     C_MAC    = 4'd1,
                     C_PREP   = 4'd2,
                     C_SQ_SIG = 4'd3,
                     C_SQ_REF = 4'd4,
                     C_NORM   = 4'd5,
                     C_SHIFT  = 4'd6,
                     C_CORDIC = 4'd7,
                     C_DIVGO  = 4'd8,
                     C_DIV    = 4'd9,
                     C_FINISH = 4'd10;

    reg [3:0] state;

    //--------------------------------------------------------------------------
    // Reference table: ph is the sample index mod 13.
    //--------------------------------------------------------------------------

    function [31:0] frame_ref_word;
        input [3:0] ph;
        reg [31:0] frame_ref;
        begin
`include "frame_sync_ref.vh"
            frame_ref_word = frame_ref;
        end
    endfunction

    //--------------------------------------------------------------------------
    // Multiply-accumulate stage
    //--------------------------------------------------------------------------

    reg [16:0] iss_left;  // addresses still to issue
    reg [3:0]  rom_ph;    // phase of the address being issued
    reg        p_valid;   // rd_data holds a sample this cycle
    reg [3:0]  p_ph;      // its phase

    reg signed [63:0] corr_re, corr_im;
    reg        [63:0] sig_power, ref_power;

    wire              issuing = (iss_left != 17'd0);

    wire [31:0]       ref_w = frame_ref_word(p_ph);
    wire signed [15:0] sr = rd_data[31:16];
    wire signed [15:0] si = rd_data[15:0];
    wire signed [15:0] rr = ref_w[31:16];
    wire signed [15:0] ri = ref_w[15:0];

    wire signed [32:0] t_re = sr * rr + si * ri;
    wire signed [32:0] t_im = si * rr - sr * ri;
    wire        [32:0] t_sp = sr * sr + si * si;  // < 2^32: unsigned
    wire        [32:0] t_rp = rr * rr + ri * ri;

    //--------------------------------------------------------------------------
    // Normalization, CORDIC and division
    //--------------------------------------------------------------------------

    reg         sq_start;
    reg  [47:0] sq_in;
    wire        sq_busy, sq_done;
    wire [23:0] sq_root;

    isqrt_seq #(.IN_W(48), .CNT_W(6)) u_sqrt (
        .clk(clk), .rst(rst), .start(sq_start), .x(sq_in),
        .busy(sq_busy), .done(sq_done), .root(sq_root)
    );

    reg  [23:0] sig_mag;
    reg  [47:0] norm;

    reg         cd_start;
    wire        cd_busy, cd_done;
    wire signed [15:0] cd_angle;
    wire signed [31:0] cd_mag;
    reg  signed [63:0] shr_re, shr_im;
    reg  [5:0]  shift;

    function signed [15:0] saturate_i16;
        input signed [63:0] v;
        begin
            if (v > 64'sd32767)
                saturate_i16 = 16'sd32767;
            else if (v < -64'sd32767)
                saturate_i16 = -16'sd32767;
            else
                saturate_i16 = v[15:0];
        end
    endfunction

    cordic u_cordic (
        .clk(clk), .rst(rst), .start(cd_start), .mode(`CORDIC_MODE_VECTOR),
        .in_a(saturate_i16(shr_re)), .in_b(saturate_i16(shr_im)),
        .busy(cd_busy), .done(cd_done), .out_a(cd_angle), .out_b(cd_mag)
    );

    reg         dv_start;
    reg  [63:0] dv_dividend;
    wire        dv_busy, dv_done;
    wire [63:0] dv_quotient;

    udiv_seq #(.WIDTH(64), .CNT_WIDTH(7)) u_div (
        .clk(clk), .rst(rst), .start(dv_start),
        .dividend(dv_dividend), .divisor({16'd0, norm}),
        .busy(dv_busy), .done(dv_done), .quotient(dv_quotient)
    );

    wire shr_out_of_range = (shr_re > 64'sd32767) || (shr_re < -64'sd32767) ||
                            (shr_im > 64'sd32767) || (shr_im < -64'sd32767);

    // Unused CORDIC outputs and handshake status.
    wire unused_ok = &{1'b0, sq_busy, cd_busy, dv_busy, cd_angle, 1'b0};

    always @(posedge clk) begin
        if (rst) begin
            state       <= C_IDLE;
            busy        <= 1'b0;
            done        <= 1'b0;
            ratio       <= 16'd0;
            rd_addr     <= {ADDR_W{1'b0}};
            iss_left    <= 17'd0;
            rom_ph      <= 4'd0;
            p_valid     <= 1'b0;
            p_ph        <= 4'd0;
            corr_re     <= 64'sd0;
            corr_im     <= 64'sd0;
            sig_power   <= 64'd0;
            ref_power   <= 64'd0;
            sq_start    <= 1'b0;
            sq_in       <= 48'd0;
            sig_mag     <= 24'd0;
            norm        <= 48'd0;
            cd_start    <= 1'b0;
            shr_re      <= 64'sd0;
            shr_im      <= 64'sd0;
            shift       <= 6'd0;
            dv_start    <= 1'b0;
            dv_dividend <= 64'd0;
        end else begin
            done     <= 1'b0;
            sq_start <= 1'b0;
            cd_start <= 1'b0;
            dv_start <= 1'b0;

            case (state)
                C_IDLE: begin
                    if (start) begin
                        busy      <= 1'b1;
                        rd_addr   <= base;
                        iss_left  <= {1'b0, len};
                        rom_ph    <= 4'd0;
                        p_valid   <= 1'b0;
                        corr_re   <= 64'sd0;
                        corr_im   <= 64'sd0;
                        sig_power <= 64'd0;
                        ref_power <= 64'd0;
                        state     <= C_MAC;
                    end
                end

                C_MAC: begin
                    if (p_valid) begin
                        corr_re   <= corr_re + {{31{t_re[32]}}, t_re};
                        corr_im   <= corr_im + {{31{t_im[32]}}, t_im};
                        sig_power <= sig_power + {31'd0, t_sp};
                        ref_power <= ref_power + {31'd0, t_rp};
                    end
                    p_valid <= issuing;
                    p_ph    <= rom_ph;
                    if (issuing) begin
                        rd_addr  <= rd_addr + {{(ADDR_W-1){1'b0}}, 1'b1};
                        rom_ph   <= (rom_ph == 4'd12) ? 4'd0 : rom_ph + 4'd1;
                        iss_left <= iss_left - 17'd1;
                    end
                    if (!issuing)  // the last sample, if any, accumulates on this edge
                        state <= C_PREP;
                end

                C_PREP: begin
                    if (sig_power == 64'd0 || ref_power == 64'd0) begin
                        ratio <= 16'd0;
                        state <= C_FINISH;
                    end else begin
                        sq_in    <= sig_power[47:0];
                        sq_start <= 1'b1;
                        state    <= C_SQ_SIG;
                    end
                end

                C_SQ_SIG: begin
                    if (sq_done) begin
                        sig_mag  <= sq_root;
                        sq_in    <= ref_power[47:0];
                        sq_start <= 1'b1;
                        state    <= C_SQ_REF;
                    end
                end

                C_SQ_REF: begin
                    if (sq_done) begin
                        norm  <= sig_mag * sq_root;
                        state <= C_NORM;
                    end
                end

                C_NORM: begin
                    if (norm == 48'd0) begin
                        ratio <= 16'd0;
                        state <= C_FINISH;
                    end else begin
                        shr_re <= corr_re;
                        shr_im <= corr_im;
                        shift  <= 6'd0;
                        state  <= C_SHIFT;
                    end
                end

                C_SHIFT: begin
                    if (shr_out_of_range && shift < 6'd48) begin
                        shr_re <= shr_re >>> 1;
                        shr_im <= shr_im >>> 1;
                        shift  <= shift + 6'd1;
                    end else begin
                        cd_start <= 1'b1;
                        state    <= C_CORDIC;
                    end
                end

                C_CORDIC: begin
                    if (cd_done) begin
                        // corr_mag << 15, 64-bit wrap as in the C++; the
                        // magnitude is sign-extended like its uint64 cast.
                        dv_dividend <= {{32{cd_mag[31]}}, cd_mag} << (shift + 6'd15);
                        state       <= C_DIVGO;
                    end
                end

                C_DIVGO: begin
                    dv_start <= 1'b1;
                    state    <= C_DIV;
                end

                C_DIV: begin
                    if (dv_done) begin
                        ratio <= (dv_quotient > 64'd65535) ? 16'd65535 : dv_quotient[15:0];
                        state <= C_FINISH;
                    end
                end

                C_FINISH: begin
                    busy  <= 1'b0;
                    done  <= 1'b1;
                    state <= C_IDLE;
                end

                default: begin
                    state <= C_IDLE;
                end
            endcase
        end
    end

endmodule
