// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// frame_correlator_tb.v — frame_correlator with the chunk RAM it reads
//
// Testbench-only wiring for test_frame_correlator.py: a write port for the
// bench to fill the RAM, and a synchronous read port feeding the
// correlator (the registered-read contract frame_correlator.v documents).

module frame_correlator_tb #(
    parameter ADDR_W = 12
) (
    input  wire              clk,
    input  wire              rst,

    input  wire              wr_en,
    input  wire [ADDR_W-1:0] wr_addr,
    input  wire [31:0]       wr_data,

    input  wire              start,
    input  wire [ADDR_W-1:0] base,
    input  wire [15:0]       len,
    output wire              busy,
    output wire              done,
    output wire [15:0]       ratio
);

    reg [31:0]        mem [0:(1<<ADDR_W)-1];
    reg [31:0]        rd_data;
    wire [ADDR_W-1:0] rd_addr;

    always @(posedge clk) begin
        if (wr_en)
            mem[wr_addr] <= wr_data;
        rd_data <= mem[rd_addr];
    end

    frame_correlator #(.ADDR_W(ADDR_W)) u_dut (
        .clk(clk), .rst(rst), .start(start), .base(base), .len(len),
        .busy(busy), .done(done), .ratio(ratio),
        .rd_addr(rd_addr), .rd_data(rd_data)
    );

endmodule
