// SPDX-License-Identifier: CERN-OHL-W-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// isqrt_seq.v — Sequential unsigned integer square root
//
// root = floor(sqrt(x)) by binary digit recurrence, one result bit per
// cycle: shifts, compares and add/subtract only. Bit-exact with
// integer_sqrt() in lib/sync/frame_sync.cc (which skips the leading zero
// digit pairs first; starting at the top pair here only adds iterations
// that leave the state unchanged).
//
// Handshake matches udiv_seq.v: with busy low, drive x and pulse start
// (sampled on that edge; ignored while busy). busy is high from that edge
// until done, which pulses for one cycle exactly IN_W/2+1 cycles later
// with root valid; root then holds until the next done. Reset clears busy,
// done and root and abandons any root in flight.
//
// IN_W must be even (>= 2); CNT_W must hold IN_W/2.

module isqrt_seq #(
    parameter IN_W  = 48,
    parameter CNT_W = 6
) (
    input  wire             clk,
    input  wire             rst,  // synchronous, active-high

    input  wire             start,
    input  wire [IN_W-1:0]  x,

    output reg              busy,
    output reg              done,  // one-cycle pulse
    output reg  [IN_W/2-1:0] root
);

    reg [IN_W-1:0]  rem;  // the C++ x, reduced as digits are accepted
    reg [IN_W-1:0]  res;
    reg [IN_W-1:0]  bit_r;
    reg [CNT_W-1:0] cnt;

    wire [IN_W-1:0] trial = res + bit_r;
    wire            fits  = rem >= trial;

    always @(posedge clk) begin
        if (rst) begin
            busy  <= 1'b0;
            done  <= 1'b0;
            root  <= {(IN_W/2){1'b0}};
            rem   <= {IN_W{1'b0}};
            res   <= {IN_W{1'b0}};
            bit_r <= {IN_W{1'b0}};
            cnt   <= {CNT_W{1'b0}};
        end else begin
            done <= 1'b0;

            if (!busy) begin
                if (start) begin
                    busy  <= 1'b1;
                    rem   <= x;
                    res   <= {IN_W{1'b0}};
                    bit_r <= {{(IN_W-1){1'b0}}, 1'b1} << (IN_W - 2);
                    cnt   <= IN_W / 2;
                end
            end else if (cnt != {CNT_W{1'b0}}) begin
                if (fits) begin
                    rem <= rem - trial;
                    res <= (res >> 1) + bit_r;
                end else begin
                    res <= res >> 1;
                end
                bit_r <= bit_r >> 2;
                cnt   <= cnt - {{(CNT_W-1){1'b0}}, 1'b1};
            end else begin
                busy <= 1'b0;
                done <= 1'b1;
                root <= res[IN_W/2-1:0];
            end
        end
    end

endmodule
