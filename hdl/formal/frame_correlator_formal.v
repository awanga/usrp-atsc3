// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// frame_correlator_formal.v — Formal harness for hdl/rtl/sync/frame_correlator.v
//
// The shared cores it drives are replaced by stubs (frame_correlator_stubs.v)
// and the datapath multipliers are cut, so this checks the correlator's own
// control against a ghost record of each accepted start (base, len):
//   - the RAM address on cycle a after the accepting edge is base + a - 1
//     for a = 1..len, i.e. exactly len consecutive addresses from base,
//     each presented once, starting the cycle after start;
//   - busy is high from that edge until done; done is a one-cycle pulse,
//     not before the samples have been read and accumulated (len + 4
//     cycles), and busy is low on it;
//   - operands are latched: base / len / start may change freely (and
//     start may be asserted) while busy without effect on the addresses;
//   - ratio only changes while busy (it is final one cycle before done) and
//     holds while idle;
//   - reset abandons a run, clears ratio and raises no stray done.
// The arithmetic (products, sums, normalization, the Q1.15 ratio) is not
// proved here: test_frame_correlator_tb.py compares every ratio bit-exactly
// with lib/sync/frame_sync.cc, and committed mutants (hdl/mutants) show
// that bench rejects a conjugate sign error, a wrong reference period, a
// missing sample, a missing shift-back and a wrong norm.
//
// Shrunk: ADDR_W=5, len 4..6. Assumption: base + len fits the address
// (the caller reads inside its RAM).
//
// Run: hdl/formal/run_formal.sh frame_correlator

module frame_correlator_formal (
    input wire clk,
    input wire rst
);

    localparam ADDR_W = 5;

    reg                start;
    reg  [ADDR_W-1:0]  base;
    reg  [15:0]        len;
    reg  [31:0]        rd_data;
    wire               busy, done;
    wire [15:0]        ratio;
    wire [ADDR_W-1:0]  rd_addr;

    frame_correlator #(.ADDR_W(ADDR_W)) dut (
        .clk(clk), .rst(rst), .start(start), .base(base), .len(len),
        .busy(busy), .done(done), .ratio(ratio),
        .rd_addr(rd_addr), .rd_data(rd_data)
    );

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;
    always @(*) if (!past_valid) assume (rst);

    wire accepted = start && !busy;

    reg                g_active;
    reg [ADDR_W-1:0]   g_base;
    reg [15:0]         g_len;
    reg [7:0]          g_age;       // cycles since the accepting edge, 1 = the next cycle
    reg [15:0]         g_ratio_prev;
    reg                g_busy_prev;
    reg                g_done_prev;
    reg                g_in_reset;

    always @(posedge clk) begin
        g_in_reset   <= rst;
        g_ratio_prev <= ratio;
        g_busy_prev  <= busy;
        g_done_prev  <= done;
        if (rst) begin
            g_active <= 1'b0;
            g_age    <= 8'd0;
        end else begin
            if (accepted) begin
                g_active <= 1'b1;
                g_base   <= base;
                g_len    <= len;
                g_age    <= 8'd1;
            end else if (g_active) begin
                if (done) g_active <= 1'b0;
                if (g_age != 8'd255) g_age <= g_age + 8'd1;
            end
        end
    end

    // Caller contract: the read stays inside the RAM, and len is a usable
    // (small, nonzero-power) value.
    always @(*) begin
        if (past_valid && !rst) begin
            assume (len >= 16'd4 && len <= 16'd6);
            assume ({1'b0, base} + {{(ADDR_W-3){1'b0}}, len[2:0]} < 6'd32);
        end
    end

    wire [ADDR_W-1:0] g_addr = g_base + g_age[ADDR_W-1:0] - {{(ADDR_W-1){1'b0}}, 1'b1};

    always @(*) begin
        if (past_valid && !rst && g_active) begin
            assert (busy || done);
            if (g_age >= 8'd1 && {8'd0, g_age} <= {8'd0, g_len[7:0]}) assert (rd_addr == g_addr);
            if (done) begin
                assert (!busy);
                assert (g_age >= {8'd0, g_len[7:0]} + 8'd4);
            end else begin
                assert (busy);
            end
        end
        if (past_valid && !rst && !g_active) assert (!busy && !done);
        if (past_valid && !rst && done) assert (!g_done_prev);
        // ratio is final one cycle before done and holds while idle.
        if (past_valid && !rst && !g_in_reset && !busy && !g_busy_prev) assert (ratio == g_ratio_prev);
        if (past_valid && g_in_reset) assert (!busy && !done && ratio == 16'd0);
    end

    always @(posedge clk) begin
        if (past_valid && !rst) begin
            cover (done);
            cover (done && ratio != 16'd0);
            cover (accepted && g_active == 1'b0 && start);
        end
    end

endmodule
