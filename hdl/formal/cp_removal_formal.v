// cp_removal_formal.v — Formal harness for cp_removal
//
// Main property (data integrity vs a ghost model): the output stream is
// exactly the input stream with the first cp_length beats of every
// symbol removed -- each output beat is the input beat accepted on the
// same cycle, unmodified (TDATA passes straight through), every non-CP
// input beat is emitted, no CP beat is, and TLAST marks exactly the
// fft_size-th output beat of each symbol. A ghost counter numbers output
// beats within the symbol and is pinned to the DUT's position counter so
// k-induction closes. Supplementary: the position counter never exceeds
// the longest defined symbol, config clamping and its flags, reset, and
// AXI4-S output stability.
//
// Assumptions, each encoding a caller contract:
//   - cfg_* are stable while not in reset (cp_removal.v's header:
//     configuration is latched at reset; reconfiguration is by reset).
//     Without it the counter bound is not even true.
//   - AXI4-S producer rule: an offered beat is held until accepted. Only
//     the output-stability check depends on it.
//
// Bounds: the deepest event, a completed symbol, needs at least
// 8192 + 192 accepted beats, far past any BMC depth; it is not covered
// here. Non-vacuity of the framing and counter properties comes from
// committed mutants (hdl/mutants: counter never wraps, TLAST one beat
// early, one extra CP beat dropped), and test_cp_removal.py exercises
// whole symbols at every CP fraction. White-box probes come from the
// flatten+expose flow; see hdl/docs/formal_conventions.md.
//
// Run: hdl/formal/run_formal.sh cp_removal

`include "axi4s_types.vh"

module cp_removal_formal (
    input wire clk,
    input wire rst
);

    localparam CNT_WIDTH = 16;
    localparam MAX_SYMBOL_LEN = `ATSC3_FFT_32K + `ATSC3_FFT_32K / 2;  // 49152

    reg  [17:0] cfg_fft_size;
    reg  [12:0] cfg_cp_fraction_numerator;
    wire        cfg_fft_size_invalid;
    wire        cfg_cp_numerator_clamped;

    reg  [31:0] s_axis_tdata;
    reg         s_axis_tvalid;
    wire        s_axis_tready;
    reg         s_axis_tlast;

    wire [31:0] m_axis_tdata;
    wire        m_axis_tvalid;
    reg         m_axis_tready;
    wire        m_axis_tlast;

    // Probes wired to the DUT's internal latched-config/position
    // registers via the ports cp_removal.sby's [script] exposed on
    // cp_removal_bare -- see that file's header.
    wire [CNT_WIDTH-1:0] cnt_probe;
    wire [CNT_WIDTH-1:0] fft_size_r_probe;
    wire [12:0]          cp_numerator_r_probe;

    cp_removal_bare dut_top (
        .clk                       (clk),
        .rst                       (rst),
        .cfg_fft_size              (cfg_fft_size),
        .cfg_cp_fraction_numerator (cfg_cp_fraction_numerator),
        .cfg_fft_size_invalid      (cfg_fft_size_invalid),
        .cfg_cp_numerator_clamped  (cfg_cp_numerator_clamped),
        .s_axis_tdata              (s_axis_tdata),
        .s_axis_tvalid             (s_axis_tvalid),
        .s_axis_tready             (s_axis_tready),
        .s_axis_tlast              (s_axis_tlast),
        .m_axis_tdata              (m_axis_tdata),
        .m_axis_tvalid             (m_axis_tvalid),
        .m_axis_tready             (m_axis_tready),
        .m_axis_tlast              (m_axis_tlast),
        .\dut.cnt            (cnt_probe),
        .\dut.fft_size_r     (fft_size_r_probe),
        .\dut.cp_numerator_r (cp_numerator_r_probe)
    );

    // s_axis_*/m_axis_tready/cfg_* are `reg` with no always-block driver
    // other than the stability assumption below: free primary inputs the
    // solver re-picks every cycle, the standard SymbiYosys stimulus
    // idiom.

    reg past_valid;
    initial past_valid = 1'b0;
    always @(posedge clk) past_valid <= 1'b1;

    always @(*) begin
        if (!past_valid) begin
            assume (rst);
        end
    end

    reg                   prev_rst;
    reg  [17:0]            prev_cfg_fft_size;
    reg  [12:0]            prev_cfg_cp_fraction_numerator;
    reg                   prev_s_valid, prev_s_ready, prev_s_tlast;
    reg  [31:0]            prev_s_tdata;
    reg                   prev_m_valid, prev_m_ready, prev_m_tlast;
    reg  [31:0]            prev_m_tdata;
    reg                   prev_cfg_fft_size_invalid, prev_cfg_cp_numerator_clamped;

    always @(posedge clk) begin
        prev_rst                       <= rst;
        prev_cfg_fft_size               <= cfg_fft_size;
        prev_cfg_cp_fraction_numerator  <= cfg_cp_fraction_numerator;
        prev_s_valid                   <= s_axis_tvalid;
        prev_s_ready                   <= s_axis_tready;
        prev_s_tdata                    <= s_axis_tdata;
        prev_s_tlast                   <= s_axis_tlast;
        prev_m_valid                   <= m_axis_tvalid;
        prev_m_ready                   <= m_axis_tready;
        prev_m_tdata                    <= m_axis_tdata;
        prev_m_tlast                   <= m_axis_tlast;
        prev_cfg_fft_size_invalid      <= cfg_fft_size_invalid;
        prev_cfg_cp_numerator_clamped  <= cfg_cp_numerator_clamped;
    end

    //--------------------------------------------------------------------
    // Config held stable while !rst (see file header) -- both current
    // and previous sample must agree whenever neither cycle is a reset.
    //--------------------------------------------------------------------
    always @(*) begin
        if (past_valid && !rst && !prev_rst) begin
            assume (cfg_fft_size == prev_cfg_fft_size);
            assume (cfg_cp_fraction_numerator == prev_cfg_cp_fraction_numerator);
        end
    end

    // Standard AXI4-S producer-compliance assumption, needed only for
    // the output-stability property below (same role as in
    // axi4s_skid_buffer_formal.v, restricted to just that property).
    always @(*) begin
        if (past_valid && !prev_rst && prev_s_valid && !prev_s_ready) begin
            assume (s_axis_tvalid);
            assume (s_axis_tdata == prev_s_tdata);
            assume (s_axis_tlast == prev_s_tlast);
        end
    end

    //--------------------------------------------------------------------
    // Reset behavior
    //--------------------------------------------------------------------
    always @(posedge clk) begin
        if (past_valid && prev_rst) begin
            assert (cnt_probe == {CNT_WIDTH{1'b0}});
        end
    end

    //--------------------------------------------------------------------
    // Clamp correctness: fft_size_r/cp_numerator_r are always in range.
    // The invalid/clamped flags only update on the cycle following a
    // reset cycle (they're written solely in cp_removal.v's `if (rst)`
    // branch, held otherwise) -- so they must exactly reflect the config
    // sampled *during* that reset cycle when prev_rst was 1, and stay
    // unchanged (held) on every other cycle.
    //--------------------------------------------------------------------
    always @(posedge clk) begin
        if (past_valid) begin
            assert (fft_size_r_probe == `ATSC3_FFT_8K || fft_size_r_probe == `ATSC3_FFT_16K ||
                    fft_size_r_probe == `ATSC3_FFT_32K);
            assert (cp_numerator_r_probe <= 13'd4096);
        end
        if (past_valid && prev_rst) begin
            assert (cfg_fft_size_invalid == !(prev_cfg_fft_size == `ATSC3_FFT_8K ||
                                               prev_cfg_fft_size == `ATSC3_FFT_16K ||
                                               prev_cfg_fft_size == `ATSC3_FFT_32K));
            assert (cfg_cp_numerator_clamped == (prev_cfg_cp_fraction_numerator > 13'd4096));
        end
        if (past_valid && !prev_rst) begin
            assert (cfg_fft_size_invalid == prev_cfg_fft_size_invalid);
            assert (cfg_cp_numerator_clamped == prev_cfg_cp_numerator_clamped);
        end
    end

    //--------------------------------------------------------------------
    // Counter bound and framing,
    // recomputed independently from the exposed registers rather than
    // re-reading the DUT's own cp_length/symbol_len/in_cp wires.
    //--------------------------------------------------------------------
    wire [2:0]            fft_scale_chk  = fft_size_r_probe[CNT_WIDTH-1:13];
    wire [CNT_WIDTH-1:0]  cp_length_chk  = cp_numerator_r_probe * fft_scale_chk;
    wire [CNT_WIDTH-1:0]  symbol_len_chk = fft_size_r_probe + cp_length_chk;
    wire                  in_cp_chk      = (cnt_probe < cp_length_chk);

    always @(*) begin
        if (past_valid) begin
            assert (cp_length_chk <= 16'd16384);
            assert (symbol_len_chk <= MAX_SYMBOL_LEN[CNT_WIDTH-1:0]);
            assert (cnt_probe < symbol_len_chk);
            assert (cnt_probe <= MAX_SYMBOL_LEN[CNT_WIDTH-1:0] - 1'b1);

            assert (s_axis_tready == (in_cp_chk ? 1'b1 : m_axis_tready));
            assert (m_axis_tvalid == (!in_cp_chk && s_axis_tvalid));
            assert (m_axis_tdata == s_axis_tdata);
            assert (m_axis_tlast == (!in_cp_chk && s_axis_tvalid &&
                                      (cnt_probe == symbol_len_chk - 1'b1)));
        end
    end

    //--------------------------------------------------------------------
    // Ghost model: output beat numbering within the symbol
    //--------------------------------------------------------------------
    reg  [CNT_WIDTH-1:0] ghost_out_idx;
    wire                 out_fire = m_axis_tvalid && m_axis_tready;
    wire                 in_fire  = s_axis_tvalid && s_axis_tready;
    always @(posedge clk) begin
        if (rst)
            ghost_out_idx <= {CNT_WIDTH{1'b0}};
        else if (out_fire)
            ghost_out_idx <= m_axis_tlast ? {CNT_WIDTH{1'b0}} : ghost_out_idx + 1'b1;
    end

    always @(*) begin
        if (past_valid && !rst) begin
            // Pinned to the position counter: no output beats yet while
            // in the CP, then one per position.
            assert (ghost_out_idx == (in_cp_chk ? {CNT_WIDTH{1'b0}} : cnt_probe - cp_length_chk));
            assert (out_fire == (in_fire && !in_cp_chk));
            if (out_fire) begin
                assert (m_axis_tdata == s_axis_tdata);
                assert (m_axis_tlast == (ghost_out_idx == fft_size_r_probe - 1'b1));
            end
        end
    end

    //--------------------------------------------------------------------
    // Black-box output stability while stalled (producer-compliance
    // assumption above), mirroring axi4s_skid_buffer_formal.v.
    //--------------------------------------------------------------------
    always @(posedge clk) begin
        if (past_valid && !prev_rst && prev_m_valid && !prev_m_ready) begin
            assert (m_axis_tvalid);
            assert (m_axis_tdata == prev_m_tdata);
            assert (m_axis_tlast == prev_m_tlast);
        end
    end

    //--------------------------------------------------------------------
    // Non-vacuity: every interesting mode is actually reachable.
    //--------------------------------------------------------------------
    // m_axis_tvalid && m_axis_tlast (completing a whole symbol) is
    // deliberately not covered here: the smallest real cp_length is 192
    // (CpFraction::k192_8192 at FFT_8K), so reaching it needs a BMC trace
    // hundreds to tens of thousands of cycles deep depending on config --
    // impractical to unroll and not what formal is for here (see
    // hdl/docs/formal_conventions.md's small-parameterization section:
    // "bit-exact numerical correctness is cocotb's job... not formal's",
    // and the same reasoning extends to "ran a whole realistic symbol").
    // test_cp_removal.py's cocotb suite exercises this concretely, at
    // real cp_length/fft_size, dozens of times over.
    always @(posedge clk) begin
        if (past_valid) begin
            cover (in_cp_chk);
            cover (!in_cp_chk && m_axis_tvalid);
            cover (cfg_fft_size_invalid);
            cover (cfg_cp_numerator_clamped);
            cover (fft_size_r_probe == `ATSC3_FFT_16K);
            cover (fft_size_r_probe == `ATSC3_FFT_32K);
        end
    end

endmodule
