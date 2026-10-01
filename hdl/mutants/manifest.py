"""RTL mutants: deliberately broken copies of hdl/rtl/ that the named
checkers must reject. Each mutant replaces exactly one occurrence of
`find` in `file` (relative to hdl/rtl/) with `replace`; a `find` that no
longer matches exactly once is reported as STALE (an error), so the
manifest cannot silently rot as the RTL changes.

`killers` lists every checker that must fail on the mutant:
  "sim:<block>"     cocotb testbench test_<block>.py (Verilator)
  "formal:<job>"    hdl/formal/<job>.sby (smtbmc prove/cover tasks)
  "bmc:<job>"       hdl/formal/prove_pdr.sh --mutant <job> (bounded ABC bmc3)

Run with hdl/mutants/run_mutants.py.
"""

MUTANTS = [
    # --- axi4s_skid_buffer -------------------------------------------------
    dict(
        name="skid_drops_stalled_beat",
        file="common/axi4s_skid_buffer.v",
        find="                skid_valid <= 1'b1;",
        replace="                skid_valid <= 1'b0;",
        killers=["sim:axi4s_skid_buffer", "formal:axi4s_skid_buffer"],
    ),
    dict(
        name="skid_loses_tlast",
        file="common/axi4s_skid_buffer.v",
        find="                m_axis_tlast  <= skid_tlast;",
        replace="                m_axis_tlast  <= 1'b0;",
        killers=["sim:axi4s_skid_buffer", "formal:axi4s_skid_buffer"],
    ),
    dict(
        name="skid_repeats_beat",
        file="common/axi4s_skid_buffer.v",
        find="            m_axis_tvalid <= 1'b0;\n        end\n    end",
        replace="            m_axis_tvalid <= 1'b1;\n        end\n    end",
        killers=["sim:axi4s_skid_buffer", "formal:axi4s_skid_buffer"],
    ),
    # --- udiv_seq ----------------------------------------------------------
    dict(
        name="udiv_one_step_short",
        file="common/udiv_seq.v",
        find="cnt       <= WIDTH[CNT_WIDTH-1:0];",
        replace="cnt       <= WIDTH[CNT_WIDTH-1:0] - 1'b1;",
        killers=["sim:udiv_seq", "formal:udiv_seq"],
    ),
    dict(
        name="udiv_strict_compare",
        file="common/udiv_seq.v",
        find="wire           fits      = ~rem_sub[WIDTH];",
        replace="wire           fits      = ~rem_sub[WIDTH] && (rem_sub != {(WIDTH+1){1'b0}});",
        killers=["sim:udiv_seq", "formal:udiv_seq"],
    ),
    dict(
        name="udiv_clears_result_on_start",
        file="common/udiv_seq.v",
        find="                    busy      <= 1'b1;",
        replace=(
            "                    busy      <= 1'b1;\n"
            "                    quotient  <= {WIDTH{1'b0}};"
        ),
        killers=["sim:udiv_seq", "formal:udiv_seq"],
    ),
    # --- cordic ------------------------------------------------------------
    dict(
        name="cordic_skips_last_iteration",
        file="common/cordic.v",
        find="if (iter == `CORDIC_ITERATIONS - 1) begin",
        replace="if (iter == `CORDIC_ITERATIONS - 2) begin",
        killers=["sim:cordic", "formal:cordic"],
    ),
    dict(
        name="cordic_loses_zero_bypass",
        file="common/cordic.v",
        find="special_zero_r <= vector_zero;",
        replace="special_zero_r <= 1'b0;",
        killers=["sim:cordic"],
    ),
    dict(
        name="cordic_busy_drops_in_prep",
        file="common/cordic.v",
        find="                    iter  <= 4'd0;\n",
        replace="                    iter  <= 4'd0;\n                    busy  <= 1'b0;\n",
        killers=["sim:cordic", "formal:cordic"],
    ),
    # --- bootstrap_detector -----------------------------------------------
    dict(
        name="bootstrap_history_wrap_late",
        file="sync/bootstrap_detector.v",
        find="({1'b0, hidx} == win_n - 1'b1)",
        replace="({1'b0, hidx} == win_n)",
        killers=["sim:bootstrap_detector", "bmc:bootstrap_detector"],
    ),
    dict(
        name="bootstrap_divides_below_floor",
        file="sync/bootstrap_detector.v",
        find="                    if (energetic) begin",
        replace="                    if (1'b1) begin",
        killers=["sim:bootstrap_detector", "bmc:bootstrap_detector"],
    ),
    dict(
        name="bootstrap_no_rearm_hysteresis",
        file="sync/bootstrap_detector.v",
        find="                            in_det        <= 1'b0;\n"
        "                            rearm_blocked <= 1'b1;",
        replace="                            in_det        <= 1'b0;\n"
        "                            rearm_blocked <= 1'b0;",
        killers=["sim:bootstrap_detector"],
    ),
    dict(
        name="bootstrap_stays_in_detection",
        file="sync/bootstrap_detector.v",
        find="                            in_det        <= 1'b0;\n"
        "                            rearm_blocked <= 1'b1;",
        replace="                            in_det        <= 1'b1;\n"
        "                            rearm_blocked <= 1'b1;",
        killers=["bmc:bootstrap_detector"],
    ),
    dict(
        name="bootstrap_sample_count_skips",
        file="sync/bootstrap_detector.v",
        find="sample_count <= sample_count + 48'd1;",
        replace="sample_count <= sample_count + 48'd2;",
        killers=["sim:bootstrap_detector", "bmc:bootstrap_detector"],
    ),
    dict(
        name="bootstrap_energy_floor_low",
        file="sync/bootstrap_detector.v",
        find="MIN_ENERGY  = 64'sd65536;",
        replace="MIN_ENERGY  = 64'sd32768;",
        killers=["sim:bootstrap_detector"],
    ),
    # --- polyphase_fir -----------------------------------------------------
    dict(
        name="fir_no_rounding",
        file="sync/polyphase_fir.v",
        find="ROUND_BIAS = 40'sd16384;",
        replace="ROUND_BIAS = 40'sd0;",
        killers=["sim:polyphase_fir", "sim:timing_recovery"],
    ),
    # (Shifting only tap 0's address is a near-equivalent mutant: tap 0's
    # coefficient is 0 or +-1 in every phase.)
    dict(
        name="fir_tap_step_skipped",
        file="sync/polyphase_fir.v",
        find="mem_addr <= tap_addr - 1'b1;  // tap_addr for t_reg+1",
        replace="mem_addr <= tap_addr;  // tap_addr for t_reg+1",
        killers=["sim:polyphase_fir", "sim:timing_recovery"],
    ),
    # --- timing_recovery ---------------------------------------------------
    dict(
        name="timing_mu_reset_not_doubled",
        file="sync/timing_recovery.v",
        find="mu_q16   <= {cfg_initial_offset_q15[14:0], 1'b0};",
        replace="mu_q16   <= cfg_initial_offset_q15;",
        killers=["sim:timing_recovery", "formal:timing_recovery"],
    ),
    dict(
        name="timing_buffer_write_slot_skewed",
        file="sync/timing_recovery.v",
        find="                    buf_wr_addr <= buf_write_idx;",
        replace="                    buf_wr_addr <= buf_write_idx + 1'b1;",
        killers=["sim:timing_recovery", "formal:timing_recovery"],
    ),
    dict(
        name="timing_ted_mid_phase",
        file="sync/timing_recovery.v",
        find="fir_phase    <= mu_snapshot_plus_half[15:12];",
        replace="fir_phase    <= mu_snapshot[15:12];",
        killers=["sim:timing_recovery"],
    ),
    dict(
        name="timing_ted_diff_zero_extended",
        file="sync/timing_recovery.v",
        find="wire signed [16:0] diff_re = x_curr_re - x_prev_re;",
        replace="wire signed [16:0] diff_re = {1'b0, x_curr_re} - {1'b0, x_prev_re};",
        killers=["sim:timing_recovery"],
    ),
    # --- cp_removal --------------------------------------------------------
    dict(
        name="cp_counter_never_wraps",
        file="ofdm/cp_removal.v",
        find="cnt <= in_last ? {CNT_WIDTH{1'b0}} : cnt + 1'b1;",
        replace="cnt <= cnt + 1'b1;",
        killers=["sim:cp_removal", "formal:cp_removal"],
    ),
    dict(
        name="cp_tlast_early",
        file="ofdm/cp_removal.v",
        find="wire in_last = (cnt == symbol_len - 1'b1);",
        replace="wire in_last = (cnt == symbol_len - {{(CNT_WIDTH-2){1'b0}}, 2'd2});",
        killers=["sim:cp_removal", "formal:cp_removal"],
    ),
    dict(
        name="cp_drops_one_extra_sample",
        file="ofdm/cp_removal.v",
        find="wire in_cp   = (cnt < cp_length);",
        replace="wire in_cp   = (cnt <= cp_length);",
        killers=["sim:cp_removal", "formal:cp_removal"],
    ),
    # --- fft_engine --------------------------------------------------------
    dict(
        name="fft_unload_skips_settle",
        file="ofdm/fft_engine.v",
        find="                    ram_addr_a <= sample_cnt;\n"
        "                    state      <= ST_UNLOAD_WAIT;",
        replace="                    ram_addr_a <= sample_cnt;\n"
        "                    state      <= ST_UNLOAD_EMIT;",
        killers=["sim:fft_engine"],
    ),
    dict(
        name="fft_bitrev_shift_short",
        file="ofdm/fft_engine.v",
        find="bitrev_full >> (ADDR_WIDTH[3:0] - log2_n_r);",
        replace="bitrev_full >> (ADDR_WIDTH[3:0] - log2_n_r - 4'd1);",
        killers=["sim:fft_engine", "formal:fft_engine"],
    ),
    dict(
        name="fft_no_tlast",
        file="ofdm/fft_engine.v",
        find="m_axis_tlast  <= (sample_cnt == fft_size_r[ADDR_WIDTH-1:0] - 1'b1);",
        replace="m_axis_tlast  <= 1'b0;",
        killers=["sim:fft_engine"],
    ),
]
