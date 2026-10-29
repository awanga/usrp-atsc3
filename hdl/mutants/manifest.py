# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Alfred Wanga
#
"""RTL mutants: deliberately broken copies of hdl/rtl/ that the named
checkers must reject. Each mutant replaces exactly one occurrence of
`find` in `file` (relative to hdl/rtl/) with `replace`; a `find` that no
longer matches exactly once is reported as STALE (an error), so the
manifest cannot silently rot as the RTL changes.

`killers` lists every checker that must fail on the mutant:
  "sim:<block>"     cocotb testbench test_<block>.py (Verilator)
  "bench:<bench>"   cocotb integration bench test_<bench>.py (Verilator)
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
    # --- freq_correction ---------------------------------------------------
    dict(
        name="fc_phase_reset_after_sample",
        file="sync/freq_correction.v",
        find="cordic_angle <= acc_base[31:16];",
        replace="cordic_angle <= acc[31:16];",
        killers=["sim:freq_correction", "formal:freq_correction"],
    ),
    dict(
        name="fc_no_negative_saturation",
        file="sync/freq_correction.v",
        find="                sat16 = -16'sd32767;",
        replace="                sat16 = v[15:0];",
        killers=["sim:freq_correction", "formal:freq_correction"],
    ),
    dict(
        name="fc_overwrites_stalled_output",
        file="sync/freq_correction.v",
        find="                    if (out_free) begin",
        replace="                    if (1'b1) begin",
        killers=["sim:freq_correction", "formal:freq_correction"],
    ),
    dict(
        name="fc_loses_tlast",
        file="sync/freq_correction.v",
        find="m_axis_tlast  <= in_last;",
        replace="m_axis_tlast  <= 1'b0;",
        killers=["sim:freq_correction", "formal:freq_correction"],
    ),
    # --- pilot_extractor ---------------------------------------------------
    dict(
        name="pe_continual_beats_scattered",
        file="ofdm/pilot_extractor.v",
        find="wire [7:0]  pilot_type = is_sp ? `PILOT_TYPE_SCATTERED :\n"
        "                             is_cp ? `PILOT_TYPE_CONTINUAL : `PILOT_TYPE_EDGE;",
        replace="wire [7:0]  pilot_type = is_cp ? `PILOT_TYPE_CONTINUAL :\n"
        "                             is_sp ? `PILOT_TYPE_SCATTERED : `PILOT_TYPE_EDGE;",
        killers=["sim:pilot_extractor", "formal:pilot_extractor"],
    ),
    dict(
        name="pe_live_symbol_index",
        file="ofdm/pilot_extractor.v",
        find="wire [3:0]  sym       = first_bin ? s_axis_tuser : sym_r;",
        replace="wire [3:0]  sym       = s_axis_tuser;",
        killers=["sim:pilot_extractor", "formal:pilot_extractor"],
    ),
    dict(
        name="pe_cp_pointer_skips",
        file="ofdm/pilot_extractor.v",
        find="cp_ptr_next = cp_ptr + 8'd1;",
        replace="cp_ptr_next = cp_ptr + 8'd2;",
        killers=["sim:pilot_extractor", "formal:pilot_extractor"],
    ),
    dict(
        name="pe_drops_stalled_record",
        file="ofdm/pilot_extractor.v",
        find="            end else if (m_axis_tready) begin\n"
        "                m_axis_tvalid <= 1'b0;",
        replace="            end else begin\n                m_axis_tvalid <= 1'b0;",
        killers=["sim:pilot_extractor", "formal:pilot_extractor"],
    ),
    dict(
        name="pe_lfsr_wrong_tap",
        file="ofdm/pilot_extractor.v",
        find="lfsr <= {lfsr[4] ^ lfsr[3] ^ lfsr[1] ^ lfsr[0], lfsr[12:1]};",
        replace="lfsr <= {lfsr[4] ^ lfsr[3] ^ lfsr[2] ^ lfsr[0], lfsr[12:1]};",
        killers=["sim:pilot_extractor"],
    ),
    dict(
        name="pe_cp_shift_ignored",
        file="ofdm/pilot_extractor.v",
        find="({1'b0, rel} + {6'd0, shift_r}) : {6'd0, shift_r};",
        replace="{1'b0, rel} : 16'd0;",
        killers=["sim:pilot_extractor"],
    ),
    dict(
        name="pe_additional_cp_dropped",
        file="ofdm/pilot_extractor.v",
        find="wire        is_cp     = cp_hit || add_hit;",
        replace="wire        is_cp     = cp_hit;",
        killers=["sim:pilot_extractor"],
    ),
    dict(
        name="pe_cp_stride_ignored",
        file="ofdm/pilot_extractor.v",
        find="wire [15:0] cp_abs = cp_raw >> stride_sh_r;",
        replace="wire [15:0] cp_abs = cp_raw >> 2'd2;",
        killers=["sim:pilot_extractor"],
    ),
    dict(
        name="pe_lfsr_not_reloaded",
        file="ofdm/pilot_extractor.v",
        find="                    m    <= 7'd0;\n                    lfsr <= LFSR_INIT;\n",
        replace="                    m    <= 7'd0;\n",
        killers=["sim:pilot_extractor"],
    ),
    dict(
        name="pe_lfsr_steps_before_window",
        file="ofdm/pilot_extractor.v",
        find="                    if (started)\n                        lfsr <=",
        replace="                    if (1'b1)\n                        lfsr <=",
        killers=["sim:pilot_extractor"],
    ),
    dict(
        name="pe_reference_sign_flipped",
        file="ofdm/pilot_extractor.v",
        find="lfsr[0] ? Q15_MINUS_ONE : Q15_ONE",
        replace="lfsr[0] ? Q15_ONE : Q15_MINUS_ONE",
        killers=["sim:pilot_extractor"],
    ),
    # --- isqrt_seq ---------------------------------------------------------
    dict(
        name="isqrt_strict_compare",
        file="common/isqrt_seq.v",
        find="wire            fits  = rem >= trial;",
        replace="wire            fits  = rem > trial;",
        killers=["sim:isqrt_seq", "formal:isqrt_seq"],
    ),
    dict(
        name="isqrt_skips_top_pair",
        file="common/isqrt_seq.v",
        find="bit_r <= {{(IN_W-1){1'b0}}, 1'b1} << (IN_W - 2);",
        replace="bit_r <= {{(IN_W-1){1'b0}}, 1'b1} << (IN_W - 4);",
        killers=["sim:isqrt_seq", "formal:isqrt_seq"],
    ),
    # --- frame_correlator --------------------------------------------------
    dict(
        name="fcorr_conjugate_sign",
        file="sync/frame_correlator.v",
        find="wire signed [32:0] t_im = si * rr - sr * ri;",
        replace="wire signed [32:0] t_im = si * rr + sr * ri;",
        killers=["bench:frame_correlator_tb", "sim:frame_sync"],
    ),
    dict(
        name="fcorr_reference_period_12",
        file="sync/frame_correlator.v",
        find="rom_ph   <= (rom_ph == 4'd12) ? 4'd0 : rom_ph + 4'd1;",
        replace="rom_ph   <= (rom_ph == 4'd11) ? 4'd0 : rom_ph + 4'd1;",
        killers=["bench:frame_correlator_tb", "sim:frame_sync"],
    ),
    dict(
        name="fcorr_one_sample_short",
        file="sync/frame_correlator.v",
        find="iss_left  <= {1'b0, len};",
        replace="iss_left  <= {1'b0, len} - 17'd1;",
        killers=["bench:frame_correlator_tb", "sim:frame_sync", "formal:frame_correlator"],
    ),
    dict(
        name="fcorr_no_shift_back",
        file="sync/frame_correlator.v",
        find="<< (shift + 6'd15);",
        replace="<< 6'd15;",
        killers=["bench:frame_correlator_tb", "sim:frame_sync"],
    ),
    dict(
        name="fcorr_shift_cap_8",
        file="sync/frame_correlator.v",
        find="shift < 6'd48",
        replace="shift < 6'd8",
        killers=["bench:frame_correlator_tb"],
    ),
    dict(
        name="fcorr_norm_uses_sig_twice",
        file="sync/frame_correlator.v",
        find="norm  <= sig_mag * sq_root;",
        replace="norm  <= sig_mag * sig_mag;",
        killers=["bench:frame_correlator_tb", "sim:frame_sync"],
    ),
    # --- frame_sync_fsm / frame_sync ---------------------------------------
    dict(
        name="fsync_acquiring_window_ends_preamble",
        file="sync/frame_sync_fsm.v",
        find="if (off + fft32 <= n_r && off < end_r) begin\n"
        "                        corr_start  <= 1'b1;\n"
        "                        corr_offset <= off;\n"
        "                        state       <= S_ACQ_WAIT;",
        replace="if (off + fft32 <= end_r && off < n_r) begin\n"
        "                        corr_start  <= 1'b1;\n"
        "                        corr_offset <= off;\n"
        "                        state       <= S_ACQ_WAIT;",
        killers=["sim:frame_sync"],
    ),
    dict(
        name="fsync_acquiring_ties_pick_last",
        file="sync/frame_sync_fsm.v",
        find="                        if (corr_ratio > best) begin\n"
        "                            best     <= corr_ratio;\n"
        "                            best_off <= off;\n"
        "                        end\n"
        "                        off   <= off + 32'd1;\n"
        "                        state <= S_ACQ_CHK;",
        replace="                        if (corr_ratio >= best) begin\n"
        "                            best     <= corr_ratio;\n"
        "                            best_off <= off;\n"
        "                        end\n"
        "                        off   <= off + 32'd1;\n"
        "                        state <= S_ACQ_CHK;",
        killers=["sim:frame_sync"],
    ),
    dict(
        name="fsync_locked_ties_pick_last",
        file="sync/frame_sync_fsm.v",
        find="                        if (corr_ratio > best) begin\n"
        "                            best     <= corr_ratio;\n"
        "                            best_off <= off;\n"
        "                        end\n"
        "                        off   <= off + 32'd1;\n"
        "                        state <= S_LCK_CHK;",
        replace="                        if (corr_ratio >= best) begin\n"
        "                            best     <= corr_ratio;\n"
        "                            best_off <= off;\n"
        "                        end\n"
        "                        off   <= off + 32'd1;\n"
        "                        state <= S_LCK_CHK;",
        killers=["sim:frame_sync"],
    ),
    dict(
        name="fsync_relaxed_threshold_three_quarters",
        file="sync/frame_sync_fsm.v",
        find="wire [15:0] relax_now = (thr_now * 4) / 5;",
        replace="wire [15:0] relax_now = (thr_now * 3) / 4;",
        killers=["sim:frame_sync"],
    ),
    dict(
        name="fsync_event_old_confidence",
        file="sync/frame_sync_fsm.v",
        find="ev_confidence   <= blend95[15:0];",
        replace="ev_confidence   <= confidence;",
        killers=["sim:frame_sync"],
    ),
    dict(
        name="fsync_holdover_no_decay",
        file="sync/frame_sync_fsm.v",
        find="confidence   <= decay90[15:0];",
        replace="confidence   <= confidence;",
        killers=["sim:frame_sync"],
    ),
    dict(
        name="fsync_locked_boundary_no_frame",
        file="sync/frame_sync_fsm.v",
        find="expected        <= sample_counter + {16'd0, best_off} + frame_samples;",
        replace="expected        <= sample_counter + {16'd0, best_off};",
        killers=["sim:frame_sync"],
    ),
    dict(
        name="fsync_search_step_eighth",
        file="sync/frame_sync_fsm.v",
        find="off   <= off + {18'd0, fft_r[15:2]};  // F / 4",
        replace="off   <= off + {19'd0, fft_r[15:3]};",
        killers=["sim:frame_sync"],
    ),
    dict(
        name="fsync_bootstrap_ignored_in_holdover",
        file="sync/frame_sync_fsm.v",
        find="if (fs_state == FS_SEARCHING || fs_state == FS_HOLDOVER) begin",
        replace="if (fs_state == FS_SEARCHING) begin",
        killers=["sim:frame_sync", "formal:frame_sync_fsm"],
    ),
    dict(
        name="fsync_holdover_counter_kept",
        file="sync/frame_sync_fsm.v",
        find="                            fs_state <= FS_HOLDOVER;\n"
        "                            hold_cnt <= 32'd0;",
        replace="                            fs_state <= FS_HOLDOVER;",
        killers=["sim:frame_sync"],
    ),
    dict(
        name="fsync_lock_count_off_by_one",
        file="sync/frame_sync_fsm.v",
        find="if (hits + 32'd1 >= {16'd0, acq_r}) begin",
        replace="if (hits >= {16'd0, acq_r}) begin",
        killers=["sim:frame_sync"],
    ),
    dict(
        name="fsync_relock_keeps_misses",
        file="sync/frame_sync_fsm.v",
        find="                        fs_state        <= FS_LOCKED;\n"
        "                        misses          <= 32'd0;\n"
        "                        confidence      <= SCORE_ONE;\n"
        "                        hits            <= 32'd1;",
        replace="                        fs_state        <= FS_LOCKED;\n"
        "                        confidence      <= SCORE_ONE;\n"
        "                        hits            <= 32'd1;",
        killers=["sim:frame_sync"],
    ),
    dict(
        name="fsync_threshold_read_live",
        file="sync/frame_sync_fsm.v",
        find="                        if (corr_ratio >= thr_r) begin\n"
        "                            expected   <= sample_counter + {16'd0, off};",
        replace="                        if (corr_ratio >= thr_now) begin\n"
        "                            expected   <= sample_counter + {16'd0, off};",
        killers=["sim:frame_sync"],
    ),
    dict(
        name="fsync_event_loses_tlast",
        file="sync/frame_sync.v",
        find="m_axis_tlast                                                            <= 1'b1;",
        replace="m_axis_tlast                                                            <= 1'b0;",
        killers=["sim:frame_sync", "formal:frame_sync"],
    ),
    dict(
        name="fsync_full_ram_chunk_refused",
        file="sync/frame_sync.v",
        find="wire        proc_skip  = fft_bad || (proc_n > DEPTH);",
        replace="wire        proc_skip  = fft_bad || (proc_n >= DEPTH);",
        killers=["sim:frame_sync"],
    ),
    dict(
        name="fsync_bootstrap_taken_mid_chunk",
        file="sync/frame_sync.v",
        find="assign bs_ready = (top_state == T_RX) && at_start;",
        replace="assign bs_ready = (top_state == T_RX);",
        killers=["sim:frame_sync", "formal:frame_sync"],
    ),
    dict(
        name="fsync_sample_taken_with_bootstrap",
        file="sync/frame_sync.v",
        find="wire rx_ok = (top_state == T_RX) && !(bs_valid && at_start);",
        replace="wire rx_ok = (top_state == T_RX);",
        killers=["sim:frame_sync", "formal:frame_sync"],
    ),
    dict(
        name="fsync_search_reads_past_chunk",
        file="sync/frame_sync_fsm.v",
        find="                    if (off + fft32 <= n_r) begin\n"
        "                        corr_start  <= 1'b1;\n"
        "                        corr_offset <= off;\n"
        "                        state       <= S_SRCH_WAIT;",
        replace="                    if (off + fft32 <= n_r + 32'd1) begin\n"
        "                        corr_start  <= 1'b1;\n"
        "                        corr_offset <= off;\n"
        "                        state       <= S_SRCH_WAIT;",
        killers=["sim:frame_sync", "formal:frame_sync_fsm"],
    ),
    dict(
        name="fsync_frame_number_double_step",
        file="sync/frame_sync_fsm.v",
        find="                        frame_number    <= frame_number + 32'd1;",
        replace="                        frame_number    <= frame_number + 32'd2;",
        killers=["sim:frame_sync", "formal:frame_sync_fsm"],
    ),
]
