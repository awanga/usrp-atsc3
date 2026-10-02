// freq_correction.v — carrier-frequency-offset correction NCO
//
// Bit-exact port of lib/sync/freq_correction.cc's FreqCorrection::process()
// (ATSC3_FIXED_POINT=ON). Per sample:
//   (cos, sin) = cordic_rotate(acc[31:16])      Q1.15 angle format
//   out_re = sat((re * cos - im * sin) >>> 15)  saturate to +/-32767
//   out_im = sat((re * sin + im * cos) >>> 15)
//   acc    = acc + phase_inc                    mod 2^32
// acc and phase_inc are Q1.31 turns-over-pi. Turning a CFO estimate in Hz
// into phase_inc (and the fine-CFO loop filter) is the C++'s control
// plane, in double precision; software writes the result here, the same
// value FreqCorrection::get_phase_increment_q31() reports.
//
// Sequential: one shared cordic.v rotation per sample, so a sample takes
// CORDIC_ITERATIONS + 4 = 18 cycles from acceptance to a valid output.
//
// Interface contract:
//   - cfg_phase_inc is sampled on the edge that accepts a beat; the
//     accumulator advance after that beat uses it (the C++ applies an
//     increment set between process() calls from the next sample on).
//   - phase_reset (pulse) clears the accumulator on the edge it is high,
//     before any beat accepted on that same edge (reset_phase() then
//     process()).
//   - s_axis_tready is high only when no sample is in flight: one sample
//     at a time. An output beat is valid 18 cycles after its input was
//     accepted at the earliest, later if the previous output is still
//     stalled, and is held, unchanged, until accepted. TLAST passes
//     through with its sample.
//   - Reset (synchronous, active-high) clears the accumulator, the output
//     and any sample in flight.
//
// AXI4-S: TDATA=ci16 TVALID TREADY TLAST in and out.

`include "axi4s_types.vh"
`include "cordic_types.vh"

module freq_correction (
    input  wire               clk,
    input  wire               rst,  // synchronous, active-high

    input  wire signed [31:0] cfg_phase_inc,  // Q1.31 turns-over-pi per sample
    input  wire               phase_reset,

    `AXI4S_SLAVE(s_axis, `ATSC3_SAMPLE_WIDTH),
    `AXI4S_MASTER(m_axis, `ATSC3_SAMPLE_WIDTH)
);

    localparam ST_IDLE   = 2'd0,
               ST_ROTATE = 2'd1,  // waiting for the CORDIC
               ST_OUT    = 2'd2;  // products registered; waiting for the output

    localparam signed [17:0] SAT_MAX = 18'sd32767;
    localparam signed [17:0] SAT_MIN = -18'sd32767;

    reg [1:0]         state;
    reg [31:0]        acc;
    reg signed [15:0] in_re, in_im;
    reg               in_last;

    wire fire_in = s_axis_tvalid && s_axis_tready;
    always @(*) begin
        s_axis_tready = (state == ST_IDLE);
    end

    wire [31:0] acc_base = phase_reset ? 32'd0 : acc;

    //--------------------------------------------------------------------
    // CORDIC rotation of the accumulator's top 16 bits
    //--------------------------------------------------------------------

    reg                cordic_start;
    reg signed [15:0]  cordic_angle;
    wire               cordic_busy, cordic_done;
    wire signed [15:0] cos_q15;
    wire signed [31:0] sin_q15_wide;

    cordic u_cordic (
        .clk   (clk),
        .rst   (rst),
        .start (cordic_start),
        .mode  (`CORDIC_MODE_ROTATE),
        .in_a  (cordic_angle),
        .in_b  (16'sd0),
        .busy  (cordic_busy),
        .done  (cordic_done),
        .out_a (cos_q15),
        .out_b (sin_q15_wide)
    );
    // Rotation mode's sin is a sign-extended Q1.15 value (cordic.v).
    wire signed [15:0] sin_q15 = sin_q15_wide[15:0];

    //--------------------------------------------------------------------
    // Complex multiply, shift and saturate
    //--------------------------------------------------------------------

    reg signed [31:0] p_re_cos, p_im_sin, p_re_sin, p_im_cos;
    wire signed [32:0] sum_re = {p_re_cos[31], p_re_cos} - {p_im_sin[31], p_im_sin};
    wire signed [32:0] sum_im = {p_re_sin[31], p_re_sin} + {p_im_cos[31], p_im_cos};
    wire signed [17:0] shr_re = sum_re[32:15];  // arithmetic >> 15
    wire signed [17:0] shr_im = sum_im[32:15];

    function [15:0] sat16;
        input signed [17:0] v;
        begin
            if (v > SAT_MAX)
                sat16 = 16'sd32767;
            else if (v < SAT_MIN)
                sat16 = -16'sd32767;
            else
                sat16 = v[15:0];
        end
    endfunction

    //--------------------------------------------------------------------
    // Control
    //--------------------------------------------------------------------

    wire out_free = !m_axis_tvalid || m_axis_tready;

    always @(posedge clk) begin
        if (rst) begin
            state         <= ST_IDLE;
            acc           <= 32'd0;
            cordic_start  <= 1'b0;
            cordic_angle  <= 16'sd0;
            in_re         <= 16'sd0;
            in_im         <= 16'sd0;
            in_last       <= 1'b0;
            p_re_cos      <= 32'sd0;
            p_im_sin      <= 32'sd0;
            p_re_sin      <= 32'sd0;
            p_im_cos      <= 32'sd0;
            m_axis_tvalid <= 1'b0;
            m_axis_tdata  <= {`ATSC3_SAMPLE_WIDTH{1'b0}};
            m_axis_tlast  <= 1'b0;
        end else begin
            cordic_start <= 1'b0;
            if (m_axis_tvalid && m_axis_tready)
                m_axis_tvalid <= 1'b0;

            if (fire_in)
                acc <= acc_base + cfg_phase_inc;
            else if (phase_reset)
                acc <= 32'd0;

            case (state)
                ST_IDLE: begin
                    if (fire_in) begin
                        in_re        <= s_axis_tdata[31:16];
                        in_im        <= s_axis_tdata[15:0];
                        in_last      <= s_axis_tlast;
                        cordic_angle <= acc_base[31:16];
                        cordic_start <= 1'b1;
                        state        <= ST_ROTATE;
                    end
                end
                ST_ROTATE: begin
                    if (cordic_done) begin
                        p_re_cos <= in_re * cos_q15;
                        p_im_sin <= in_im * sin_q15;
                        p_re_sin <= in_re * sin_q15;
                        p_im_cos <= in_im * cos_q15;
                        state    <= ST_OUT;
                    end
                end
                default: begin  // ST_OUT
                    if (out_free) begin
                        m_axis_tvalid <= 1'b1;
                        m_axis_tdata  <= {sat16(shr_re), sat16(shr_im)};
                        m_axis_tlast  <= in_last;
                        state         <= ST_IDLE;
                    end
                end
            endcase
        end
    end

    // Unused by design: busy (start is only pulsed from ST_IDLE, when the
    // core is idle), the sign extension of sin, and the bits the >>> 15
    // discards.
    wire unused_ok = &{1'b0, cordic_busy, sin_q15_wide[31:16], sum_re[14:0], sum_im[14:0], 1'b0};

endmodule
