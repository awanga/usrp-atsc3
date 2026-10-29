// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// isqrt_seq_formal.v — Formal harness for hdl/rtl/common/isqrt_seq.v
//
// Main property (data integrity vs a ghost model): the operand is captured
// on the edge that accepts start; when done pulses, root must satisfy
// root^2 <= x < (root+1)^2 -- the definition of floor(sqrt(x)), checked by
// multiplication, not by reimplementing the recurrence -- done must arrive
// exactly IN_W/2+1 cycles after that edge, and root must hold until the
// next result. Supplementary: busy is high for exactly that window, done is
// a one-cycle pulse, and reset aborts a root without a stray done.
//
// Shrunk to IN_W=8 (the algorithm is width-generic; 48-bit operands are
// exercised in test_isqrt_seq.py). One root takes 5 cycles, so depth 12
// covers a full root plus a back-to-back one; with every input free this
// is an exhaustive proof for all 256 operands at that width.
//
// Inputs are unconstrained: start may be asserted at any time (the RTL
// ignores it while busy, which is part of what is checked), the operand
// may change every cycle. Only the first cycle is constrained to reset.
//
// Run: hdl/formal/run_formal.sh isqrt_seq

module isqrt_seq_formal (
    input wire clk,
    input wire rst
);

    localparam IN_W  = 8;
    localparam CNT_W = 3;
    localparam LAT   = IN_W / 2 + 1;

    reg               start;
    reg  [IN_W-1:0]   x;
    wire              busy;
    wire              done;
    wire [IN_W/2-1:0] root;

    isqrt_seq #(.IN_W(IN_W), .CNT_W(CNT_W)) dut (
        .clk(clk), .rst(rst), .start(start), .x(x),
        .busy(busy), .done(done), .root(root)
    );

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;
    always @(*) if (!past_valid) assume (rst);

    // Ghost model: operand and elapsed cycles of the root in flight.
    reg               g_active;
    reg [IN_W-1:0]    g_x;
    reg [3:0]         g_age;
    reg [IN_W/2-1:0]  g_last_root;
    reg               g_have_root;
    wire              accepted = start && !busy;

    always @(posedge clk) begin
        if (rst) begin
            g_active    <= 1'b0;
            g_age       <= 4'd0;
            g_have_root <= 1'b0;
        end else begin
            if (accepted) begin
                g_active <= 1'b1;
                g_x      <= x;
                g_age    <= 4'd0;  // edges since the accepting edge
            end else if (g_active) begin
                g_age <= g_age + 4'd1;
            end
            if (done) begin
                g_last_root <= root;
                g_have_root <= 1'b1;
                if (!accepted) g_active <= 1'b0;
            end
        end
    end

    wire [IN_W-1:0] root_w   = {{(IN_W/2){1'b0}}, root};
    wire [2*IN_W-1:0] r_sq   = root_w * root_w;
    wire [2*IN_W-1:0] r1_sq  = (root_w + 1) * (root_w + 1);

    reg prev_rst;
    always @(posedge clk) prev_rst <= rst;

    always @(*) begin
        if (past_valid && !rst) begin
            if (done) begin
                assert (g_active);
                assert (g_age == LAT);
                assert (r_sq <= {{IN_W{1'b0}}, g_x});
                assert ({{IN_W{1'b0}}, g_x} < r1_sq);
            end
            if (g_active && g_age < LAT) assert (busy && !done);
            if (!g_active) assert (!busy);
            if (g_have_root && !done) assert (root == g_last_root);
        end
        if (past_valid && prev_rst) assert (!busy && !done);
    end

    always @(posedge clk) begin
        if (past_valid && !rst) begin
            cover (done && root != {(IN_W/2){1'b0}});
            cover (done && g_x == {IN_W{1'b1}});
            cover (done && accepted);  // back-to-back start on the done cycle
        end
    end

endmodule
