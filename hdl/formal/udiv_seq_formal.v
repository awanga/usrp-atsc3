// udiv_seq_formal.v — Formal harness for hdl/rtl/common/udiv_seq.v
//
// Main property (data integrity vs a ghost model): a ghost copy of the
// operands is captured on the edge that accepts start; when done pulses,
// quotient must equal ghost_dividend / ghost_divisor (all ones for a zero
// divisor), done must arrive exactly WIDTH+1 cycles after that edge, and
// quotient must hold until the next result. Supplementary: busy is high
// for exactly that window, done is a one-cycle pulse, and reset aborts a
// division without a stray done.
//
// Shrunk to WIDTH=4 (the algorithm is width-generic; 16-bit operands are
// exercised in test_udiv_seq.py). One division takes WIDTH+1 = 5 cycles,
// so depth 12 covers a full division plus a back-to-back one; k-induction
// at that depth closes without extra invariants because every window that
// ends on a done cycle contains the start edge that produced it.
//
// Inputs are unconstrained: start may be asserted at any time (the RTL
// ignores it while busy, which is part of what is checked), operands may
// change every cycle. Only the first cycle is constrained to reset.
//
// Run: hdl/formal/run_formal.sh udiv_seq

module udiv_seq_formal (
    input wire clk,
    input wire rst
);

    localparam WIDTH     = 4;
    localparam CNT_WIDTH = 3;

    reg              start;
    reg  [WIDTH-1:0] dividend;
    reg  [WIDTH-1:0] divisor;
    wire             busy;
    wire             done;
    wire [WIDTH-1:0] quotient;

    udiv_seq #(.WIDTH(WIDTH), .CNT_WIDTH(CNT_WIDTH)) dut (
        .clk(clk), .rst(rst), .start(start), .dividend(dividend), .divisor(divisor),
        .busy(busy), .done(done), .quotient(quotient)
    );

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;
    always @(*) if (!past_valid) assume (rst);

    // Ghost model: operands and elapsed cycles of the division in flight.
    reg             g_active;
    reg [WIDTH-1:0] g_a, g_b;
    reg [3:0]       g_age;
    reg [WIDTH-1:0] g_last_q;
    reg             g_have_q;
    wire            accepted = start && !busy;

    always @(posedge clk) begin
        if (rst) begin
            g_active <= 1'b0;
            g_age    <= 4'd0;
            g_have_q <= 1'b0;
        end else begin
            if (accepted) begin
                g_active <= 1'b1;
                g_a      <= dividend;
                g_b      <= divisor;
                g_age    <= 4'd0;  // edges since the accepting edge
            end else if (g_active) begin
                g_age <= g_age + 4'd1;
            end
            if (done) begin
                g_last_q <= quotient;
                g_have_q <= 1'b1;
                if (!accepted) g_active <= 1'b0;
            end
        end
    end

    wire [WIDTH-1:0] g_expected = (g_b == {WIDTH{1'b0}}) ? {WIDTH{1'b1}} : g_a / g_b;

    reg prev_rst;
    always @(posedge clk) prev_rst <= rst;

    always @(*) begin
        if (past_valid && !rst) begin
            // Latency and result.
            if (done) begin
                assert (g_active);
                assert (g_age == WIDTH + 1);
                assert (quotient == g_expected);
            end
            if (g_active && g_age <= WIDTH) assert (busy && !done);
            if (!g_active) assert (!busy);
            // Result held until the next one.
            if (g_have_q && !done) assert (quotient == g_last_q);
        end
        if (past_valid && prev_rst) assert (!busy && !done);
    end

    always @(posedge clk) begin
        if (past_valid && !rst) begin
            cover (done && g_b != {WIDTH{1'b0}} && quotient != {WIDTH{1'b0}});
            cover (done && g_b == {WIDTH{1'b0}});
            cover (done && accepted);  // back-to-back start on the done cycle
        end
    end

endmodule
