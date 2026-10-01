// qualify.v — trivial harness for qualifying formal solvers
//
// A 2-bit counter that wraps at 3. With SHOULD_FAIL=0 the assertion
// (cnt never reaches 3) is true forever and
// inductive; with SHOULD_FAIL=1 the asserted bound is wrong and the
// counter reaches it at step 4. A qualified solver must report PASS for
// the first and a counterexample for the second, within the time limit.

module qualify #(
    parameter SHOULD_FAIL = 0
) (
    input wire clk
);
    reg [1:0] cnt;
    initial cnt = 2'd0;
    always @(posedge clk) cnt <= (cnt == 2'd2) ? 2'd0 : cnt + 2'd1;

    always @(*) begin
        if (SHOULD_FAIL)
            assert (cnt != 2'd2);
        else
            assert (cnt != 2'd3);
    end
endmodule
