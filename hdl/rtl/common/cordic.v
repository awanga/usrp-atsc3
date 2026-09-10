// cordic.v — Shared fixed-point CORDIC core (rotation + vectoring modes)
//
// Iterative, not spatially pipelined: one iteration's worth of
// adder/shifter hardware is reused across all 14 steps (a 4-bit iteration
// counter selects the shift amount and ROM entry each cycle) rather than
// replicating the datapath 14 times. Throughput is not yet budgeted, so
// the simpler, easier-to-verify iterative FSM is the right choice here,
// not a spatial pipeline -- revisit once timing-closure work begins.
// Bit-exact port of lib/dsp/cordic.cc's cordic_rotate()/cordic_vector():
// every intermediate register width mirrors that file's int32_t/int16_t
// choices directly (x/y/z kept at a full 32 bits throughout, exactly
// matching the C++ locals) rather than attempting a tighter width -- no
// width-optimization attempted yet either, for the same reason.
//
// Not an AXI4-S block itself (matches lib/dsp/cordic.h's own header note):
// a start/busy/done handshake instead, meant to be instantiated inside a
// streaming block's own AXI4-S FSM (see hdl/rtl/sync/bootstrap_detector.v,
// the first consumer).
//
// Usage: with busy low, drive mode/in_a/in_b and pulse start for exactly
// one cycle (hold in_a/in_b stable that cycle; they are sampled on the
// clock edge start is high). `done` pulses for exactly one cycle some
// bounded number of cycles later (see cordic.sby's cover trace for the
// exact count at the time of writing -- not hardcoded here since it is not
// part of this module's documented contract, only its behavior), with
// out_a/out_b valid that same cycle and stable until the next `start`.
//   ROTATE (`CORDIC_MODE_ROTATE`): in_a = theta (Q1.15 angle format, see
//     lib/dsp/cordic.h), in_b unused. out_a = cos(theta) (Q1.15), out_b =
//     sin(theta) (Q1.15, sign-extended to 32 bits).
//   VECTOR (`CORDIC_MODE_VECTOR`): in_a = x (Q1.15), in_b = y (Q1.15).
//     out_a = atan2(y, x) (Q1.15 angle format), out_b = magnitude (int32
//     range, per lib/dsp/cordic.h's CordicVectorResult::magnitude --
//     callers saturate/rescale to their own block's expected range).

`include "cordic_types.vh"

module cordic (
    input  wire               clk,
    input  wire                rst,   // synchronous, active-high

    input  wire                start, // pulse; sampled only while !busy
    input  wire                mode,  // `CORDIC_MODE_ROTATE / `CORDIC_MODE_VECTOR
    input  wire signed [15:0]  in_a,
    input  wire signed [15:0]  in_b,

    output reg                 busy,
    output reg                 done,  // one-cycle pulse
    output reg  signed [15:0]  out_a,
    output reg  signed [31:0]  out_b
);

    localparam ST_IDLE   = 2'd0,
               ST_PREP   = 2'd1,
               ST_ITER   = 2'd2,
               ST_FINISH = 2'd3;

    reg [1:0] state;
    reg [3:0] iter;  // 0..`CORDIC_ITERATIONS-1
    reg       mode_r;
    reg       flip_r;
    reg       special_zero_r;

    reg signed [31:0] x, y, z;

    reg [15:0] atan_table [0:`CORDIC_ITERATIONS-1];
    initial begin
        atan_table[0]  = 16'sd8192;
        atan_table[1]  = 16'sd4836;
        atan_table[2]  = 16'sd2555;
        atan_table[3]  = 16'sd1297;
        atan_table[4]  = 16'sd651;
        atan_table[5]  = 16'sd325;
        atan_table[6]  = 16'sd162;
        atan_table[7]  = 16'sd81;
        atan_table[8]  = 16'sd40;
        atan_table[9]  = 16'sd20;
        atan_table[10] = 16'sd10;
        atan_table[11] = 16'sd5;
        atan_table[12] = 16'sd2;
        atan_table[13] = 16'sd1;
    end

    // saturate_i16 / saturating_negate / add_half_turn, matching cordic.cc
    // exactly (see that file's comments for the derivation of each).
    function signed [15:0] saturate_i16;
        input signed [31:0] v;
        begin
            if (v > 32767)
                saturate_i16 = 16'sd32767;
            else if (v < -32767)
                saturate_i16 = -16'sd32767;
            else
                saturate_i16 = v[15:0];
        end
    endfunction

    function signed [15:0] saturating_negate;
        input signed [15:0] v;
        begin
            if (v == -16'sd32768)
                saturating_negate = 16'sd32767;
            else
                saturating_negate = -v;
        end
    endfunction

    function signed [15:0] add_half_turn;
        input signed [15:0] angle;
        begin
            // +0x8000 mod 2^16 == flipping bit 15 -- see cordic.cc's
            // cordic_add_half_turn() comment for why this is exact.
            add_half_turn = {~angle[15], angle[14:0]};
        end
    endfunction

    // PREP-stage combinational decisions, evaluated off the live in_a/in_b
    // inputs (sampled into registers only when start actually fires).
    wire rotate_flip = (in_a > 16'sd16384) || (in_a < -16'sd16384);
    wire signed [15:0] rotate_theta_reduced = rotate_flip ? add_half_turn(in_a) : in_a;

    wire vector_flip = in_a[15];
    wire vector_zero = (in_a == 16'sd0) && (in_b == 16'sd0);

    // Shared per-iteration update. Rotate drives d off z's sign; vector
    // drives it off y's sign (see cordic.cc's cordic_rotate()/
    // cordic_vector() -- the x/y/z update formulas are otherwise
    // identical between the two modes, which is exactly why one datapath
    // serves both).
    wire d_is_minus1 = (mode_r == `CORDIC_MODE_ROTATE) ? z[31] : ~y[31];
    wire signed [31:0] shifted_y = y >>> iter;
    wire signed [31:0] shifted_x = x >>> iter;
    wire signed [31:0] atan_val  = {{16{atan_table[iter][15]}}, atan_table[iter]};

    wire signed [31:0] x_next = d_is_minus1 ? (x + shifted_y) : (x - shifted_y);
    wire signed [31:0] y_next = d_is_minus1 ? (y - shifted_x) : (y + shifted_x);
    wire signed [31:0] z_next = d_is_minus1 ? (z + atan_val)  : (z - atan_val);

    // FINISH-stage combinational post-processing, precomputed as plain
    // wires off the final x/y/z so the FINISH state body is a simple mux.
    wire signed [15:0] sat_x = saturate_i16(x);
    wire signed [15:0] sat_y = saturate_i16(y);
    wire signed [15:0] sat_z = saturate_i16(z);
    wire signed [15:0] rotate_cos   = flip_r ? saturating_negate(sat_x) : sat_x;
    wire signed [15:0] rotate_sin   = flip_r ? saturating_negate(sat_y) : sat_y;
    wire signed [15:0] vector_angle = flip_r ? add_half_turn(sat_z) : sat_z;
    // x is gain-scaled by K (~1.6468); rescale to a true magnitude. x is at
    // most ~54000 in magnitude (32767 * K) here, times a Q1.15 constant
    // (~2e4) -- comfortably inside the 32-bit result width; only the low 32
    // bits of the underlying multiply are kept, matching cordic.cc's
    // truncating static_cast<int32_t>(magnitude_wide) exactly (both simply
    // drop the same high bits of the same wide product).
    // Explicit sign-extension via replication (same idiom this file already
    // uses for atan_val/in_a/in_b below) rather than a bare widening
    // assignment, which Verilator flags as an implicit-width mismatch even
    // though it's a correct sign-extension for a signed literal.
    wire signed [15:0] inv_gain_16 = `CORDIC_INV_GAIN_Q15;
    wire signed [31:0] inv_gain_32 = {{16{inv_gain_16[15]}}, inv_gain_16};
    wire signed [31:0] vector_mag_wide = ($signed(x) * inv_gain_32) >>> 15;

    always @(posedge clk) begin
        if (rst) begin
            state          <= ST_IDLE;
            busy           <= 1'b0;
            done           <= 1'b0;
            iter           <= 4'd0;
            mode_r         <= 1'b0;
            flip_r         <= 1'b0;
            special_zero_r <= 1'b0;
            x <= 32'sd0;
            y <= 32'sd0;
            z <= 32'sd0;
            out_a <= 16'sd0;
            out_b <= 32'sd0;
        end else begin
            done <= 1'b0;

            case (state)
                ST_IDLE: begin
                    busy <= 1'b0;
                    if (start) begin
                        busy   <= 1'b1;
                        mode_r <= mode;
                        state  <= ST_PREP;
                        if (mode == `CORDIC_MODE_ROTATE) begin
                            flip_r         <= rotate_flip;
                            special_zero_r <= 1'b0;
                            x <= inv_gain_32;
                            y <= 32'sd0;
                            z <= {{16{rotate_theta_reduced[15]}}, rotate_theta_reduced};
                        end else begin
                            flip_r         <= vector_flip;
                            special_zero_r <= vector_zero;
                            x <= vector_flip ? -{{16{in_a[15]}}, in_a} : {{16{in_a[15]}}, in_a};
                            y <= vector_flip ? -{{16{in_b[15]}}, in_b} : {{16{in_b[15]}}, in_b};
                            z <= 32'sd0;
                        end
                    end
                end

                ST_PREP: begin
                    // x/y/z (registered above) are already valid by this
                    // cycle; this state exists to decide the degenerate
                    // vector (0,0) bypass (see cordic_vector()'s early
                    // return) before committing to the iteration loop.
                    iter  <= 4'd0;
                    state <= special_zero_r ? ST_FINISH : ST_ITER;
                end

                ST_ITER: begin
                    x <= x_next;
                    y <= y_next;
                    z <= z_next;
                    if (iter == `CORDIC_ITERATIONS - 1) begin
                        state <= ST_FINISH;
                    end else begin
                        iter <= iter + 4'd1;
                    end
                end

                ST_FINISH: begin
                    busy  <= 1'b0;
                    done  <= 1'b1;
                    state <= ST_IDLE;
                    if (special_zero_r) begin
                        out_a <= 16'sd0;
                        out_b <= 32'sd0;
                    end else if (mode_r == `CORDIC_MODE_ROTATE) begin
                        out_a <= rotate_cos;
                        out_b <= {{16{rotate_sin[15]}}, rotate_sin};
                    end else begin
                        out_a <= vector_angle;
                        out_b <= vector_mag_wide[31:0];
                    end
                end

                default: state <= ST_IDLE;
            endcase
        end
    end

endmodule
