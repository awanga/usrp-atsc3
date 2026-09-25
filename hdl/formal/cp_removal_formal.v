// cp_removal_formal.v — Formal harness top for cp_removal
//
// Verification-only. Not synthesized, not part of rtl/. Instantiates the
// flatten+expose-generated cp_removal_bare (produced earlier in
// cp_removal.sby's [script] -- see that file, cp_removal_bare.v, and
// hdl/docs/formal_conventions.md for why this two-stage flow is used
// instead of `bind` or a hierarchical dotted reference).
//
// cfg_fft_size/cfg_cp_fraction_numerator are free but held STABLE while
// !rst (see cp_removal.v's header on why config is latched-at-reset, not
// read live): a real system only ever reconfigures by resetting the
// block, and the counter-bound property below isn't even true without
// this assumption (a live config could shrink symbol_len below the
// current cnt with no clock edge in between). Everything else is fully
// free every cycle, the standard SymbiYosys stimulus idiom.
//
// Properties, all in terms of the exposed \dut.cnt / \dut.fft_size_r /
// \dut.cp_numerator_r plus the port-level signals, with cp_length/
// symbol_len/in_cp recomputed here from the same trivial formula
// cp_removal.v uses (an independent restatement, not a re-read of the
// DUT's own wires) so the proof isn't just checking self-consistency:
//   - fft_size_r is always one of {8K, 16K, 32K}; cp_numerator_r <= 4096
//     (clamp correctness).
//   - cfg_fft_size_invalid / cfg_cp_numerator_clamped exactly reflect
//     whether the config presented the cycle before latching (i.e. the
//     cycle before the most recent reset) was out of range.
//   - cnt < symbol_len always, and cnt <= MAX_SYMBOL_LEN - 1 always --
//     the TASKS.md "counter never exceeds the max defined length"
//     requirement, restated as two steps (the second follows from the
//     first plus symbol_len's own bound, but is asserted directly too).
//   - s_axis_tready / m_axis_tvalid / m_axis_tdata / m_axis_tlast match
//     the documented combinational rule.
//   - Output stability while stalled (assuming a compliant producer),
//     mirroring axi4s_skid_buffer_formal.v's black-box property.
//   - Reset clears cnt.
//
// Non-vacuity: the `cover` task reaches every listed property except the
// deliberately-excluded "completed a whole symbol" case (see the
// [tasks] cover block's comment). As a further sanity check (not part of
// the checked-in flow), removing the wraparound in cp_removal.v's
// cnt-update ("cnt <= cnt + 1" unconditionally, dropping the
// in_last-triggered reset to 0) still passes `prove`'s BMC base case at
// depth 20 -- the counter hasn't run far enough to overflow yet -- but
// correctly fails k-induction on the cnt <= MAX_SYMBOL_LEN - 1 property,
// confirming both that the invariant is genuinely load-bearing and that
// `mode prove` (BMC + induction), not bare `mode bmc`, is required here.
//
// Run: cd hdl/formal && sby -f cp_removal.sby

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
    // Counter bound -- the TASKS.md requirement -- and framing,
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
