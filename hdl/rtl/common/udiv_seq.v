// udiv_seq.v — Sequential unsigned restoring divider
//
// quotient = floor(dividend / divisor), one quotient bit per cycle, WIDTH
// cycles per division. Unsigned floor is the same as C/C++ truncation
// toward zero for non-negative operands. Callers that need C++'s signed
// `/` (truncate toward zero) divide magnitudes here and negate the result
// themselves -- see hdl/rtl/sync/bootstrap_detector.v.
//
// Iterative rather than one combinational divide: a WIDTH-bit
// combinational divider is WIDTH chained subtract/compare stages, far
// too deep for a single cycle at any realistic WIDTH. Throughput is not
// yet budgeted (radix-4, early termination on leading zeros, or a
// pipelined array are the obvious upgrades once timing-closure work
// starts).
//
// Handshake matches hdl/rtl/common/cordic.v: with busy low, drive
// dividend/divisor and pulse start for one cycle (operands are sampled on
// that edge). `done` pulses for one cycle exactly WIDTH+1 cycles later,
// with `quotient` valid that cycle and held until the next start.
//
// divisor == 0 yields an all-ones quotient (what the restoring algorithm
// naturally produces). This module does not guard against it: callers
// must, and bootstrap_detector.v's formal harness asserts it never
// happens there.

module udiv_seq #(
    parameter WIDTH = 64,
    parameter CNT_WIDTH = 7  // must hold WIDTH; no $clog2 in 1364-2001
) (
    input  wire             clk,
    input  wire             rst,   // synchronous, active-high

    input  wire             start,
    input  wire [WIDTH-1:0] dividend,
    input  wire [WIDTH-1:0] divisor,

    output reg              busy,
    output reg              done,  // one-cycle pulse
    output reg  [WIDTH-1:0] quotient
);

    reg [WIDTH-1:0]     divisor_r;
    reg [WIDTH-1:0]     quo;  // dividend shifts out the top, quotient bits shift in
    reg [WIDTH-1:0]     rem;
    reg [CNT_WIDTH-1:0] cnt;

    // rem < divisor always holds between steps, so the shifted partial
    // remainder fits in WIDTH+1 bits.
    wire [WIDTH:0] rem_shift = {rem, quo[WIDTH-1]};
    wire [WIDTH:0] rem_sub   = rem_shift - {1'b0, divisor_r};
    wire           fits      = ~rem_sub[WIDTH];  // rem_shift >= divisor_r

    always @(posedge clk) begin
        if (rst) begin
            busy      <= 1'b0;
            done      <= 1'b0;
            quotient  <= {WIDTH{1'b0}};
            divisor_r <= {WIDTH{1'b0}};
            quo       <= {WIDTH{1'b0}};
            rem       <= {WIDTH{1'b0}};
            cnt       <= {CNT_WIDTH{1'b0}};
        end else begin
            done <= 1'b0;

            if (!busy) begin
                if (start) begin
                    busy      <= 1'b1;
                    divisor_r <= divisor;
                    quo       <= dividend;
                    rem       <= {WIDTH{1'b0}};
                    cnt       <= WIDTH[CNT_WIDTH-1:0];
                end
            end else if (cnt != {CNT_WIDTH{1'b0}}) begin
                rem <= fits ? rem_sub[WIDTH-1:0] : rem_shift[WIDTH-1:0];
                quo <= {quo[WIDTH-2:0], fits};
                cnt <= cnt - {{(CNT_WIDTH-1){1'b0}}, 1'b1};
            end else begin
                busy     <= 1'b0;
                done     <= 1'b1;
                quotient <= quo;
            end
        end
    end

endmodule
