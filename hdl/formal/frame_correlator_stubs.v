// frame_correlator_stubs.v — formal-only stand-ins for the shared cores
// frame_correlator instantiates
//
// The harness (frame_correlator_formal.v) checks the correlator's own
// control: addressing, handshakes, latency, holds and reset. The cores it
// drives are proved or tested separately (udiv_seq and cordic have their
// own harnesses, isqrt_seq likewise, and test_frame_correlator_tb.py
// checks the whole datapath bit-exactly against the model), so here each
// is replaced by a module with the same ports and handshake contract and
// arbitrary results: busy from an accepted start until a done pulse that
// comes at a solver-chosen time at least one cycle later, outputs
// arbitrary, start ignored while busy, reset clears busy and done.
//
// Verification-only. Not synthesized, not part of rtl/.

module isqrt_seq #(
    parameter IN_W  = 48,
    parameter CNT_W = 6
) (
    input  wire              clk,
    input  wire              rst,
    input  wire              start,
    input  wire [IN_W-1:0]   x,
    output reg               busy,
    output reg               done,
    output reg  [IN_W/2-1:0] root
);
    reg go;                    // free: when the core finishes
    reg [IN_W/2-1:0] result;   // free: what it returns
    wire unused = &{1'b0, x, CNT_W[0], 1'b0};
    always @(posedge clk) begin
        if (rst) begin
            busy <= 1'b0;
            done <= 1'b0;
            root <= {(IN_W/2){1'b0}};
        end else begin
            done <= 1'b0;
            if (!busy) begin
                if (start) busy <= 1'b1;
            end else if (go) begin
                busy <= 1'b0;
                done <= 1'b1;
                root <= result;
            end
        end
    end
endmodule

module cordic (
    input  wire               clk,
    input  wire               rst,
    input  wire               start,
    input  wire               mode,
    input  wire signed [15:0] in_a,
    input  wire signed [15:0] in_b,
    output reg                busy,
    output reg                done,
    output reg  signed [15:0] out_a,
    output reg  signed [31:0] out_b
);
    reg go;
    reg signed [15:0] result_a;
    reg signed [31:0] result_b;
    wire unused = &{1'b0, mode, in_a, in_b, 1'b0};
    always @(posedge clk) begin
        if (rst) begin
            busy  <= 1'b0;
            done  <= 1'b0;
            out_a <= 16'sd0;
            out_b <= 32'sd0;
        end else begin
            done <= 1'b0;
            if (!busy) begin
                if (start) busy <= 1'b1;
            end else if (go) begin
                busy  <= 1'b0;
                done  <= 1'b1;
                out_a <= result_a;
                out_b <= result_b;
            end
        end
    end
endmodule

module udiv_seq #(
    parameter WIDTH     = 64,
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
    reg go;
    reg [WIDTH-1:0] result;
    wire unused = &{1'b0, dividend, divisor, CNT_WIDTH[0], 1'b0};
    always @(posedge clk) begin
        if (rst) begin
            busy     <= 1'b0;
            done     <= 1'b0;
            quotient <= {WIDTH{1'b0}};
        end else begin
            done <= 1'b0;
            if (!busy) begin
                if (start) busy <= 1'b1;
            end else if (go) begin
                busy     <= 1'b0;
                done     <= 1'b1;
                quotient <= result;
            end
        end
    end
endmodule
