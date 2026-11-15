// SPDX-License-Identifier: CERN-OHL-W-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// h_average.v — temporal IIR smoothing of the channel estimate
//
// Bit-exact port of lib/channel/channel_estimator.cc apply_averaging()
// (ATSC3_FIXED_POINT=ON). Per carrier and rail, once a previous symbol
// has been stored:
//
//   out = clamp((alpha * in + (32768 - alpha) * prev) >> 15, +/-32767)
//
// with alpha in Q15 (0 .. 32768) and an arithmetic shift (floor). The
// first symbol after reset is stored and passed through unchanged. With
// cfg_enable low the stage is a pass-through and stores nothing, as the
// C++ skips the call.
//
// The previous estimate lives in a 2^ADDR_W x 32 RAM with a registered
// read; carrier n of every symbol uses address n, so num_active must not
// exceed 2^ADDR_W.
//
// Interface contract:
//   - Input/output are positional ci16 streams (beat n is relative
//     carrier n), TLAST on the symbol's last carrier. One beat of
//     latency in the stage register, no combinational path from input to
//     output. Output is held until accepted.
//   - cfg_enable and cfg_alpha are read live; change them only between
//     symbols. cfg_alpha must be <= 32768 (the owner clamps it).
//   - Reset (synchronous, active-high) forgets the stored estimate; the
//     RAM itself is not cleared (has_prev gates every read).
//
// AXI4-S: TDATA=ci16 TVALID TREADY TLAST(symbol) in and out.

`include "axi4s_types.vh"

module h_average #(
    parameter ADDR_W = 15
) (
    input  wire        clk,
    input  wire        rst,  // synchronous, active-high

    input  wire        cfg_enable,
    input  wire [15:0] cfg_alpha,  // Q15, 0 .. 32768

    input  wire [(`ATSC3_SAMPLE_WIDTH)-1:0] s_axis_tdata,
    input  wire                    s_axis_tvalid,
    output reg                     s_axis_tready,
    input  wire                    s_axis_tlast,
    output reg  [(`ATSC3_SAMPLE_WIDTH)-1:0] m_axis_tdata,
    output reg                     m_axis_tvalid,
    input  wire                    m_axis_tready,
    output reg                     m_axis_tlast
);

    reg [31:0] mem [0:(1 << ADDR_W) - 1];

    reg [ADDR_W-1:0] k;        // next input carrier
    reg              has_prev;
    reg              c_valid;  // stage register holds a beat
    reg [31:0]       in_r;
    reg              last_r;
    reg [ADDR_W-1:0] c_addr;
    reg [31:0]       prev_r;

    wire signed [15:0] in_re = in_r[31:16];
    wire signed [15:0] in_im = in_r[15:0];
    wire signed [15:0] pv_re = prev_r[31:16];
    wire signed [15:0] pv_im = prev_r[15:0];

    wire signed [16:0] alpha   = {1'b0, cfg_alpha};
    wire signed [16:0] one_m_a = 17'sd32768 - {1'b0, cfg_alpha};
    wire signed [33:0] mix_re  = alpha * in_re + one_m_a * pv_re;
    wire signed [33:0] mix_im  = alpha * in_im + one_m_a * pv_im;
    wire signed [33:0] sh_re   = mix_re >>> 15;
    wire signed [33:0] sh_im   = mix_im >>> 15;
    wire signed [15:0] av_re   = (sh_re > 34'sd32767) ? 16'sd32767 :
                                 (sh_re < -34'sd32767) ? -16'sd32767 : sh_re[15:0];
    wire signed [15:0] av_im   = (sh_im > 34'sd32767) ? 16'sd32767 :
                                 (sh_im < -34'sd32767) ? -16'sd32767 : sh_im[15:0];

    wire        use_avg = cfg_enable && has_prev;
    wire [31:0] out_w   = use_avg ? {av_re, av_im} : in_r;

    always @(*) begin
        m_axis_tvalid = c_valid;
        m_axis_tdata  = out_w;
        m_axis_tlast  = last_r;
        s_axis_tready = !c_valid || m_axis_tready;
    end

    wire s_fire = s_axis_tvalid && s_axis_tready;
    wire m_fire = c_valid && m_axis_tready;

    always @(posedge clk) begin
        if (rst) begin
            k        <= {ADDR_W{1'b0}};
            has_prev <= 1'b0;
            c_valid  <= 1'b0;
            in_r     <= 32'd0;
            last_r   <= 1'b0;
            c_addr   <= {ADDR_W{1'b0}};
            prev_r   <= 32'd0;
        end else begin
            if (m_fire) begin
                c_valid <= 1'b0;
                if (cfg_enable)
                    mem[c_addr] <= out_w;
                if (last_r && cfg_enable)
                    has_prev <= 1'b1;
            end
            if (s_fire) begin
                c_valid <= 1'b1;
                in_r    <= s_axis_tdata;
                last_r  <= s_axis_tlast;
                c_addr  <= k;
                prev_r  <= mem[k];
                k       <= s_axis_tlast ? {ADDR_W{1'b0}} : k + {{(ADDR_W-1){1'b0}}, 1'b1};
            end
        end
    end

endmodule
