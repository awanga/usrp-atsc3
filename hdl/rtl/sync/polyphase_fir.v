// polyphase_fir.v — 16-bank x 32-tap polyphase FIR interpolator core
//
// Bit-exact port of lib/sync/timing_recovery.h's
// PolyphaseInterpolator::interpolate() (ATSC3_FIXED_POINT path): given a
// phase (0..NUM_PHASES-1, selected externally from the top 4 bits of
// mu_q16) and a starting buffer index, walks NUM_TAPS samples backwards
// through an external circular sample buffer, forming a Q1.15 dot
// product against that phase's ROM taps.
//
//   acc_re = sum_t buf[(base_idx - 1 - t) mod BUF_SIZE].re * taps[phase][t]
//   acc_im = sum_t buf[(base_idx - 1 - t) mod BUF_SIZE].im * taps[phase][t]
//   result = saturate16(round((acc + 2^14) >> 15))
//
// matching interpolate()'s raw (unshifted) accumulation and single
// round-to-nearest rescale at the end (not truncation -- see that
// function's comment on why round-to-nearest was chosen for this specific
// rescale). This core does not own the sample buffer (mem_addr/
// mem_data_re/mem_data_im are a synchronous-read RAM client interface,
// one cycle of read latency assumed) -- the buffer itself lives in
// timing_recovery.v, matching the C++ class split exactly
// (PolyphaseInterpolator::interpolate() takes buf/buf_idx as plain
// arguments; it owns neither the buffer nor any index state).
//
// coeff_rom is generated from the real, running C++ filter design (see
// timing_recovery_coeffs.vh's header), not re-derived here: raised
// cosine + Kaiser window + normalization all stay one-time double-
// precision "filter design" math on the host/tooling side, the same
// treatment the CORDIC atan ROM and the FFT twiddle ROM get.
//
// Contract: with busy low, drive phase/base_idx and pulse start (sampled
// on that edge; ignored while busy). mem_addr is registered and the
// buffer is read as a synchronous RAM: mem_data must reflect the address
// presented on the previous edge (an asynchronous read also works, since
// the address is held through the following edge). busy is high from the
// start edge until done, which pulses for one cycle exactly
// 2*NUM_TAPS+1 cycles after it (2 cycles per tap plus the rounding
// cycle); result_re/result_im are valid then and hold until the next
// done. Not pipelined: timing_recovery.v makes up to 4 calls per symbol
// boundary, so a faster MAC is throughput work for later.
//
// AXI4-S: none -- a start/busy/done request/response core, the same shape
// as udiv_seq.v and cordic.v.

`include "axi4s_types.vh"

module polyphase_fir #(
    parameter NUM_PHASES     = 16,  // PolyphaseConfig::num_phases (const, see hdl_register_map.json)
    parameter NUM_TAPS       = 32,  // PolyphaseConfig::taps_per_phase (const)
    parameter PHASE_WIDTH    = 4,   // log2(NUM_PHASES)
    parameter TAP_CNT_WIDTH  = 5,   // log2(NUM_TAPS), counts 0..NUM_TAPS-1
    parameter BUF_ADDR_WIDTH = 7    // log2(BUF_SIZE), BUF_SIZE = 4 * NUM_TAPS = 128 by default
) (
    input  wire                         clk,
    input  wire                         rst,  // synchronous, active-high

    input  wire                         start,
    input  wire [PHASE_WIDTH-1:0]       phase,
    input  wire [BUF_ADDR_WIDTH-1:0]    base_idx,

    output reg  [BUF_ADDR_WIDTH-1:0]    mem_addr,
    input  wire signed [15:0]           mem_data_re,
    input  wire signed [15:0]           mem_data_im,

    output reg                          busy,
    output reg                          done,
    output reg  signed [15:0]           result_re,
    output reg  signed [15:0]           result_im
);

    //--------------------------------------------------------------------
    // Coefficient ROM: coeff_rom[phase][tap], Q1.15. Combinational read
    // (a small distributed ROM, 16*32*16 = 8Kbit) -- no extra pipeline
    // stage needed the way the external sample RAM has one.
    //--------------------------------------------------------------------

    reg signed [15:0] coeff_rom [0:NUM_PHASES-1][0:NUM_TAPS-1];
    initial begin
        `include "timing_recovery_coeffs.vh"
    end

    wire signed [15:0] tap_coeff = coeff_rom[phase][t_reg];

    //--------------------------------------------------------------------
    // Sequential MAC datapath
    //--------------------------------------------------------------------

    localparam [1:0] ST_IDLE  = 2'd0,
                     ST_ADDR  = 2'd1,
                     ST_MAC   = 2'd2,
                     ST_ROUND = 2'd3;

    reg [1:0]                    state;
    reg [TAP_CNT_WIDTH-1:0]      t_reg;
    reg [BUF_ADDR_WIDTH-1:0]     base_idx_r;
    reg signed [39:0]            acc_re, acc_im;

    wire [BUF_ADDR_WIDTH-1:0] tap_addr = base_idx_r - 1'b1 -
                                          {{(BUF_ADDR_WIDTH-TAP_CNT_WIDTH){1'b0}}, t_reg};

    localparam [TAP_CNT_WIDTH-1:0] LAST_TAP_IDX = NUM_TAPS[TAP_CNT_WIDTH-1:0] - 1'b1;
    wire last_tap = (t_reg == LAST_TAP_IDX);

    wire signed [31:0] prod_re = mem_data_re * tap_coeff;
    wire signed [31:0] prod_im = mem_data_im * tap_coeff;

    localparam signed [39:0] ROUND_BIAS = 40'sd16384;  // 1 << 14

    function signed [15:0] saturate_i16;
        input signed [39:0] v;
        begin
            if (v > 40'sd32767)
                saturate_i16 = 16'sd32767;
            else if (v < -40'sd32767)
                saturate_i16 = -16'sd32767;
            else
                saturate_i16 = v[15:0];
        end
    endfunction

    always @(posedge clk) begin
        if (rst) begin
            state      <= ST_IDLE;
            busy       <= 1'b0;
            done       <= 1'b0;
            mem_addr   <= {BUF_ADDR_WIDTH{1'b0}};
            t_reg      <= {TAP_CNT_WIDTH{1'b0}};
            base_idx_r <= {BUF_ADDR_WIDTH{1'b0}};
            acc_re     <= 40'sd0;
            acc_im     <= 40'sd0;
            result_re  <= 16'sd0;
            result_im  <= 16'sd0;
        end else begin
            done <= 1'b0;

            case (state)
                ST_IDLE: begin
                    if (start) begin
                        busy       <= 1'b1;
                        base_idx_r <= base_idx;
                        t_reg      <= {TAP_CNT_WIDTH{1'b0}};
                        acc_re     <= 40'sd0;
                        acc_im     <= 40'sd0;
                        mem_addr   <= base_idx - 1'b1;  // tap_addr for t=0
                        state      <= ST_ADDR;
                    end
                end

                // One extra cycle for the RAM's own read latency before
                // mem_data_re/im reflect mem_addr.
                ST_ADDR: begin
                    state <= ST_MAC;
                end

                ST_MAC: begin
                    acc_re <= acc_re + {{8{prod_re[31]}}, prod_re};
                    acc_im <= acc_im + {{8{prod_im[31]}}, prod_im};

                    if (last_tap) begin
                        state <= ST_ROUND;
                    end else begin
                        t_reg    <= t_reg + 1'b1;
                        mem_addr <= tap_addr - 1'b1;  // tap_addr for t_reg+1
                        state    <= ST_ADDR;
                    end
                end

                ST_ROUND: begin
                    result_re <= saturate_i16((acc_re + ROUND_BIAS) >>> 15);
                    result_im <= saturate_i16((acc_im + ROUND_BIAS) >>> 15);
                    busy      <= 1'b0;
                    done      <= 1'b1;
                    state     <= ST_IDLE;
                end

                default: state <= ST_IDLE;
            endcase
        end
    end

endmodule
