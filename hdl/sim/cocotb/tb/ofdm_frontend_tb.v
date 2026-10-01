// ofdm_frontend_tb.v — integration top: cp_removal feeding fft_engine
//
// Testbench-only wiring for test_ofdm_frontend_tb.py: the two blocks
// connected exactly as in the receiver (cp_removal's FFT-length output
// stream straight into fft_engine's load port, one clock domain). The
// sample counters are testbench instrumentation for the status checks.

module ofdm_frontend_tb #(
    parameter TWIDDLE_HEX = "fft_twiddles.hex"
) (
    input  wire        clk,
    input  wire        rst,
    input  wire [17:0] cfg_fft_size,
    input  wire [12:0] cfg_cp_fraction_numerator,
    output wire        cp_fft_size_invalid,
    output wire        cp_numerator_clamped,
    output wire        fft_size_invalid,

    input  wire [31:0] s_axis_tdata,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,

    output wire [31:0] m_axis_tdata,
    output wire        m_axis_tvalid,
    input  wire        m_axis_tready,
    output wire        m_axis_tlast,

    output reg  [31:0] cp_out_beats,   // beats cp_removal handed to fft_engine
    output reg  [31:0] cp_out_symbols  // TLASTs on that link
);

    wire [31:0] link_tdata;
    wire        link_tvalid, link_tready, link_tlast;

    cp_removal u_cp (
        .clk                       (clk),
        .rst                       (rst),
        .cfg_fft_size              (cfg_fft_size),
        .cfg_cp_fraction_numerator (cfg_cp_fraction_numerator),
        .cfg_fft_size_invalid      (cp_fft_size_invalid),
        .cfg_cp_numerator_clamped  (cp_numerator_clamped),
        .s_axis_tdata              (s_axis_tdata),
        .s_axis_tvalid             (s_axis_tvalid),
        .s_axis_tready             (s_axis_tready),
        .s_axis_tlast              (s_axis_tlast),
        .m_axis_tdata              (link_tdata),
        .m_axis_tvalid             (link_tvalid),
        .m_axis_tready             (link_tready),
        .m_axis_tlast              (link_tlast)
    );

    fft_engine #(.TWIDDLE_HEX(TWIDDLE_HEX)) u_fft (
        .clk                  (clk),
        .rst                  (rst),
        .cfg_fft_size         (cfg_fft_size),
        .cfg_fft_size_invalid (fft_size_invalid),
        .s_axis_tdata         (link_tdata),
        .s_axis_tvalid        (link_tvalid),
        .s_axis_tready        (link_tready),
        .s_axis_tlast         (link_tlast),
        .m_axis_tdata         (m_axis_tdata),
        .m_axis_tvalid        (m_axis_tvalid),
        .m_axis_tready        (m_axis_tready),
        .m_axis_tlast         (m_axis_tlast)
    );

    always @(posedge clk) begin
        if (rst) begin
            cp_out_beats   <= 32'd0;
            cp_out_symbols <= 32'd0;
        end else if (link_tvalid && link_tready) begin
            cp_out_beats   <= cp_out_beats + 32'd1;
            cp_out_symbols <= cp_out_symbols + {31'd0, link_tlast};
        end
    end

endmodule
