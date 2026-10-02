// ofdm_frontend_tb.v — integration top: freq_correction -> cp_removal ->
// fft_engine -> pilot_extractor
//
// Testbench-only wiring for test_ofdm_frontend_tb.py: the four blocks
// connected as in the receiver, one clock domain. The FFT output link is
// also brought out as a tap (fft_*) so the bench can check every FFT bin,
// not just the pilots. The symbol counter feeding the pilot extractor's
// TUSER stands in for frame sync's symbol index. The beat and symbol
// counters are testbench instrumentation for the status checks.

module ofdm_frontend_tb #(
    parameter TWIDDLE_HEX = "fft_twiddles.hex",
    parameter CP_ROM_HEX  = "pilot_cp_positions.hex"
) (
    input  wire        clk,
    input  wire        rst,
    input  wire [31:0] cfg_phase_inc,
    input  wire [17:0] cfg_fft_size,
    input  wire [12:0] cfg_cp_fraction_numerator,
    input  wire [3:0]  cfg_pilot_pattern,
    output wire        cp_fft_size_invalid,
    output wire        cp_numerator_clamped,
    output wire        fft_size_invalid,
    output wire        pe_fft_size_invalid,
    output wire        pe_pattern_invalid,
    output wire        pe_num_active_invalid,
    output wire        pe_framing_error,

    input  wire [31:0] s_axis_tdata,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,

    output wire [95:0] m_axis_tdata,
    output wire        m_axis_tvalid,
    input  wire        m_axis_tready,
    output wire        m_axis_tlast,

    output wire [31:0] fft_tdata,       // tap on the fft -> pilot link
    output wire        fft_fire,
    output wire        fft_tlast,

    output reg  [31:0] fc_out_beats,    // beats freq_correction handed on
    output reg  [31:0] cp_out_beats,    // beats cp_removal handed to fft_engine
    output reg  [31:0] cp_out_symbols,  // TLASTs on that link
    output reg  [31:0] fft_out_symbols  // TLASTs into pilot_extractor
);

    wire [31:0] fc_tdata, cp_tdata, fft_tdata_w;
    wire        fc_tvalid, fc_tready, fc_tlast;
    wire        cp_tvalid, cp_tready, cp_tlast;
    wire        fft_tvalid, fft_tready;

    freq_correction u_fc (
        .clk           (clk),
        .rst           (rst),
        .cfg_phase_inc (cfg_phase_inc),
        .phase_reset   (1'b0),
        .s_axis_tdata  (s_axis_tdata),
        .s_axis_tvalid (s_axis_tvalid),
        .s_axis_tready (s_axis_tready),
        .s_axis_tlast  (s_axis_tlast),
        .m_axis_tdata  (fc_tdata),
        .m_axis_tvalid (fc_tvalid),
        .m_axis_tready (fc_tready),
        .m_axis_tlast  (fc_tlast)
    );

    cp_removal u_cp (
        .clk                       (clk),
        .rst                       (rst),
        .cfg_fft_size              (cfg_fft_size),
        .cfg_cp_fraction_numerator (cfg_cp_fraction_numerator),
        .cfg_fft_size_invalid      (cp_fft_size_invalid),
        .cfg_cp_numerator_clamped  (cp_numerator_clamped),
        .s_axis_tdata              (fc_tdata),
        .s_axis_tvalid             (fc_tvalid),
        .s_axis_tready             (fc_tready),
        .s_axis_tlast              (fc_tlast),
        .m_axis_tdata              (cp_tdata),
        .m_axis_tvalid             (cp_tvalid),
        .m_axis_tready             (cp_tready),
        .m_axis_tlast              (cp_tlast)
    );

    fft_engine #(.TWIDDLE_HEX(TWIDDLE_HEX)) u_fft (
        .clk                  (clk),
        .rst                  (rst),
        .cfg_fft_size         (cfg_fft_size),
        .cfg_fft_size_invalid (fft_size_invalid),
        .s_axis_tdata         (cp_tdata),
        .s_axis_tvalid        (cp_tvalid),
        .s_axis_tready        (cp_tready),
        .s_axis_tlast         (cp_tlast),
        .m_axis_tdata         (fft_tdata_w),
        .m_axis_tvalid        (fft_tvalid),
        .m_axis_tready        (fft_tready),
        .m_axis_tlast         (fft_tlast)
    );

    reg [3:0] symbol_index;  // mod 16: the pilot extractor uses symbol mod Dy

    pilot_extractor #(.CP_ROM_HEX(CP_ROM_HEX)) u_pe (
        .clk                     (clk),
        .rst                     (rst),
        .cfg_fft_size            (cfg_fft_size),
        .cfg_pilot_pattern       (cfg_pilot_pattern),
        .cfg_num_active_carriers (16'd0),
        .cfg_fft_size_invalid    (pe_fft_size_invalid),
        .cfg_pattern_invalid     (pe_pattern_invalid),
        .cfg_num_active_invalid  (pe_num_active_invalid),
        .framing_error           (pe_framing_error),
        .s_axis_tdata            (fft_tdata_w),
        .s_axis_tvalid           (fft_tvalid),
        .s_axis_tready           (fft_tready),
        .s_axis_tlast            (fft_tlast),
        .s_axis_tuser            (symbol_index),
        .m_axis_tdata            (m_axis_tdata),
        .m_axis_tvalid           (m_axis_tvalid),
        .m_axis_tready           (m_axis_tready),
        .m_axis_tlast            (m_axis_tlast)
    );

    assign fft_tdata = fft_tdata_w;
    assign fft_fire  = fft_tvalid && fft_tready;

    always @(posedge clk) begin
        if (rst) begin
            fc_out_beats    <= 32'd0;
            cp_out_beats    <= 32'd0;
            cp_out_symbols  <= 32'd0;
            fft_out_symbols <= 32'd0;
            symbol_index    <= 4'd0;
        end else begin
            if (fc_tvalid && fc_tready)
                fc_out_beats <= fc_out_beats + 32'd1;
            if (cp_tvalid && cp_tready) begin
                cp_out_beats   <= cp_out_beats + 32'd1;
                cp_out_symbols <= cp_out_symbols + {31'd0, cp_tlast};
            end
            if (fft_fire && fft_tlast) begin
                fft_out_symbols <= fft_out_symbols + 32'd1;
                symbol_index    <= symbol_index + 4'd1;
            end
        end
    end

endmodule
