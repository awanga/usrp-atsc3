// SPDX-License-Identifier: CERN-OHL-W-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// complex_divider_stubs.v — formal-only stand-in for udiv_seq (complex_divider and equalizer)
//
// complex_divider_formal.v checks the divider's operand handling, sign and
// clamp logic and its handshake. The restoring divider is proved on its
// own (udiv_seq.sby), so here it is replaced by a module with the same
// ports that obeys the contract that proof establishes -- busy from the
// accepting edge until done, done one cycle long, quotient held between
// results -- with an arbitrary quotient and a fixed 3-cycle latency.
//
// The stub also asserts the caller contract the real divider documents: a
// division is never started with a zero divisor (the callers' guards keep
// it at least 1).
//
// Verification-only. Not synthesized, not part of rtl/.

module udiv_seq #(
    parameter WIDTH = 64,
    parameter CNT_WIDTH = 7
) (
    input  wire             clk,
    input  wire             rst,
    input  wire             start,
    input  wire [WIDTH-1:0] dividend,
    input  wire [WIDTH-1:0] divisor,
    output reg              busy,
    output reg              done,
    output reg  [WIDTH-1:0] quotient
);
    (* anyseq *) wire [WIDTH-1:0] free_q;
    reg [1:0] cnt;

    always @(posedge clk) begin
        if (rst) begin
            busy     <= 1'b0;
            done     <= 1'b0;
            quotient <= {WIDTH{1'b0}};
            cnt      <= 2'd0;
        end else begin
            done <= 1'b0;
            if (!busy) begin
                if (start) begin
                    busy <= 1'b1;
                    cnt  <= 2'd3;
                end
            end else if (cnt != 2'd0) begin
                cnt <= cnt - 2'd1;
            end else begin
                busy     <= 1'b0;
                done     <= 1'b1;
                quotient <= free_q;
            end
        end
    end

    always @(*) begin
        if (!rst && start && !busy) assert (divisor != {WIDTH{1'b0}});
    end

    wire unused_ok = &{1'b0, dividend, 1'b0};
endmodule
