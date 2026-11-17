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
    dict(
        name="cp_gi12_wrong_length",
        file="ofdm/cp_removal.v",
        find="4'd12:   gi_samples_now = 13'd4864;",
        replace="4'd12:   gi_samples_now = 13'd4096;",
        killers=["sim:cp_removal", "formal:cp_removal"],
    ),
    dict(
        name="cp_gi8_allowed_at_8k",
        file="ofdm/cp_removal.v",
        find="(cfg_guard_interval <= 4'd11 && fft_size_now != `ATSC3_FFT_8K) ||",
        replace="(cfg_guard_interval <= 4'd11) ||",
        killers=["sim:cp_removal", "formal:cp_removal"],
    ),
    dict(
        name="pe_first_carrier_old_formula",
        file="ofdm/pilot_extractor.v",
        find="wire [16:0] first_now_wide = (fft_now >> 1) - (({1'b0, num_active_now} - 17'd1) >> 1);",
        replace="wire [16:0] first_now_wide = (fft_now - {1'b0, num_active_now}) >> 1;",
        killers=["sim:pilot_extractor", "formal:pilot_extractor"],
    ),
    dict(
        name="ce_first_carrier_old_formula",
        file="channel/channel_estimator.v",
        find="wire [16:0] first_now = (fft_now >> 1) - (({1'b0, num_active_now} - 17'd1) >> 1);",
        replace="wire [16:0] first_now = (fft_now - {1'b0, num_active_now}) >> 1;",
        killers=["sim:channel_estimator"],
    ),
    dict(
        name="pe_boost_table_entry",
        file="ofdm/pilot_extractor.v",
        find="7'd43 : inv_sp_now = 16'd17597;",
        replace="7'd43 : inv_sp_now = 16'd21848;",
        killers=["sim:pilot_extractor"],
    ),
    dict(
        name="pe_cp_boost_like_sp",
        file="ofdm/pilot_extractor.v",
        find="wire [15:0] inv_amp    = (cp_hit && !is_sp) ? INV_A_CP : inv_sp_r;",
        replace="wire [15:0] inv_amp    = 1'b0 ? INV_A_CP : inv_sp_r;",
        killers=["sim:pilot_extractor", "formal:pilot_extractor"],
    ),
    dict(
        name="pe_additional_cp_boost_like_cp",
        file="ofdm/pilot_extractor.v",
        find="wire [15:0] inv_amp    = (cp_hit && !is_sp) ? INV_A_CP : inv_sp_r;",
        replace="wire [15:0] inv_amp    = (is_cp && !is_sp) ? INV_A_CP : inv_sp_r;",
        killers=["sim:pilot_extractor", "formal:pilot_extractor"],
    ),
    dict(
        name="pe_reserved_boost_accepted",
        file="ofdm/pilot_extractor.v",
        find="wire       boost_valid_now = (cfg_sp_boost <= 3'd4);",
        replace="wire       boost_valid_now = 1'b1;",
        killers=["sim:pilot_extractor", "formal:pilot_extractor"],
    ),
    dict(
        name="ce_ls_truncates",
        file="channel/channel_estimator.v",
        find="wire signed [31:0] r_re = (p_re + 32'sd16384) >>> 15;",
        replace="wire signed [31:0] r_re = p_re >>> 15;",
        killers=["sim:channel_estimator"],
    ),
    # --- fft_engine --------------------------------------------------------
    dict(
        name="fft_unload_natural_order",
        file="ofdm/fft_engine.v",
        find="ram_addr_a <= sample_cnt ^ fft_size_r[ADDR_WIDTH:1];",
        replace="ram_addr_a <= sample_cnt;",
        killers=["sim:fft_engine"],
    ),
    dict(
        name="fft_unload_skips_settle",
        file="ofdm/fft_engine.v",
        find="                    ram_addr_a <= sample_cnt ^ fft_size_r[ADDR_WIDTH:1];\n"
        "                    state      <= ST_UNLOAD_WAIT;",
        replace="                    ram_addr_a <= sample_cnt ^ fft_size_r[ADDR_WIDTH:1];\n"
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
        find="lfsr[0] ? 16'd0 - inv_amp : inv_amp",
        replace="lfsr[0] ? inv_amp : 16'd0 - inv_amp",
        killers=["sim:pilot_extractor"],
    ),
    # --- complex_divider ---------------------------------------------------
    dict(
        name="cd_guard_removed",
        file="channel/complex_divider.v",
        find="zero_div <= (mag_sq_w[31:15] == 17'd0);",
        replace="zero_div <= 1'b0;",
        killers=["sim:complex_divider", "formal:complex_divider"],
    ),
    dict(
        name="cd_clamp_asymmetric",
        file="channel/complex_divider.v",
        find="wire [15:0] clamped = (div_quotient > 32'd32767) ? 16'd32767 :"
        " div_quotient[15:0];",
        replace="wire [15:0] clamped = (div_quotient > 32'd32768) ? 16'd32768 :"
        " div_quotient[15:0];",
        killers=["sim:complex_divider", "formal:complex_divider"],
    ),
    dict(
        name="cd_imag_numerator_sign",
        file="channel/complex_divider.v",
        find="{{2{p_aim_bre[31]}}, p_aim_bre} - {{2{p_are_bim[31]}}, p_are_bim};",
        replace="{{2{p_aim_bre[31]}}, p_aim_bre} + {{2{p_are_bim[31]}}, p_are_bim};",
        killers=["sim:complex_divider", "formal:complex_divider"],
    ),
    dict(
        name="cd_tlast_lost",
        file="channel/complex_divider.v",
        find="last_r   <= s_axis_tlast;",
        replace="last_r   <= 1'b0;",
        killers=["sim:complex_divider", "formal:complex_divider"],
    ),
    # --- linear_interp -----------------------------------------------------
    dict(
        name="li_remainder_never_wraps",
        file="channel/linear_interp.v",
        find="wire        wrap_re = (rsum_re >= {1'b0, seg_len});",
        replace="wire        wrap_re = 1'b0;",
        killers=["sim:channel_estimator", "formal:linear_interp"],
    ),
    dict(
        name="li_falling_segment_sign_ignored",
        file="channel/linear_interp.v",
        find="neg_re  <= d_re[16];",
        replace="neg_re  <= 1'b0;",
        killers=["sim:channel_estimator", "formal:linear_interp"],
    ),
    dict(
        name="li_imag_remainder_from_real_dividend",
        file="channel/linear_interp.v",
        find="div_dividend  = (state == S_IM || state == S_IM_W) ? m_im : m_re;",
        replace="div_dividend  = (state == S_IM) ? m_im : m_re;",
        killers=["sim:channel_estimator", "formal:linear_interp"],
    ),
    dict(
        name="li_trailing_tlast_early",
        file="channel/linear_interp.v",
        find="m_axis_tlast  <= (k == max_pos_ok);",
        replace="m_axis_tlast  <= (k == max_pos_ok - 16'd1);",
        killers=["sim:channel_estimator", "formal:linear_interp"],
    ),
    # --- h_average ---------------------------------------------------------
    dict(
        name="ha_first_symbol_mixed",
        file="channel/h_average.v",
        find="wire        use_avg = cfg_enable && has_prev;",
        replace="wire        use_avg = cfg_enable;",
        killers=["sim:channel_estimator"],
    ),
    dict(
        name="ha_weights_do_not_sum_to_one",
        file="channel/h_average.v",
        find="wire signed [16:0] one_m_a = 17'sd32768 - {1'b0, cfg_alpha};",
        replace="wire signed [16:0] one_m_a = 17'sd32767 - {1'b0, cfg_alpha};",
        killers=["sim:channel_estimator"],
    ),
    dict(
        name="ha_rounds_with_wrong_shift",
        file="channel/h_average.v",
        find="wire signed [33:0] sh_im   = mix_im >>> 15;",
        replace="wire signed [33:0] sh_im   = mix_im >>> 14;",
        killers=["sim:channel_estimator"],
    ),
    # --- channel_estimator -------------------------------------------------
    dict(
        name="ce_reference_from_received_field",
        file="channel/channel_estimator.v",
        find="wire signed [15:0] inv_x = s_axis_tdata[`PILOT_SYMBOL_REFERENCE_RE_HI:`PILOT_SYMBOL_REFERENCE_RE_LO];",
        replace="wire signed [15:0] inv_x = s_axis_tdata[`PILOT_SYMBOL_RECEIVED_IM_HI:`PILOT_SYMBOL_RECEIVED_IM_LO];",
        killers=["sim:channel_estimator"],
    ),
    # --- equalizer ---------------------------------------------------------
    dict(
        name="eq_zf_structural_floor_removed",
        file="channel/equalizer.v",
        find="mag_sq_w < 32'd32768);",
        replace="1'b0);",
        killers=["sim:equalizer", "formal:equalizer"],
    ),
    dict(
        name="eq_mmse_applies_min_mag",
        file="channel/equalizer.v",
        find="wire               zero_w = mode_r ? (den_full_w < 33'd32768)",
        replace="wire               zero_w = mode_r ? ({1'b0, mag_sq_w} < {2'b00, min_mag_r})",
        killers=["sim:equalizer", "formal:equalizer"],
    ),
    dict(
        name="eq_mmse_noise_not_added",
        file="channel/equalizer.v",
        find="({1'b0, mag_sq_w} + {2'b00, noise_r})",
        replace="{1'b0, mag_sq_w}",
        killers=["sim:equalizer"],
    ),
    dict(
        name="eq_imag_numerator_sign",
        file="channel/equalizer.v",
        find="{{2{p_yim_hre[31]}}, p_yim_hre} - {{2{p_yre_him[31]}}, p_yre_him};",
        replace="{{2{p_yim_hre[31]}}, p_yim_hre} + {{2{p_yre_him[31]}}, p_yre_him};",
        killers=["sim:equalizer", "formal:equalizer"],
    ),
    dict(
        name="eq_clamp_asymmetric",
        file="channel/equalizer.v",
        find="wire [15:0] cl_re = (quo_re > 32'd32767) ? 16'd32767 : quo_re[15:0];",
        replace="wire [15:0] cl_re = (quo_re > 32'd32768) ? 16'd32768 : quo_re[15:0];",
        killers=["sim:equalizer", "formal:equalizer"],
    ),
    dict(
        name="eq_tlast_follows_y",
        file="channel/equalizer.v",
        find="last_r <= h_axis_tlast;",
        replace="last_r <= y_axis_tlast;",
        killers=["sim:equalizer", "formal:equalizer"],
    ),
    dict(
        name="eq_framing_error_never_set",
        file="channel/equalizer.v",
        find="if (y_axis_tlast != h_axis_tlast)",
        replace="if (1'b0)",
        killers=["sim:equalizer", "formal:equalizer"],
    ),
    dict(
        name="eq_zeroed_count_not_restarted",
        file="channel/equalizer.v",
        find="(sym_start ? 16'd0 : zeroed_carriers)",
        replace="zeroed_carriers",
        killers=["sim:equalizer"],
    ),
    dict(
        name="eq_accepts_y_without_h",
        file="channel/equalizer.v",
        find="y_axis_tready = (state == S_IDLE) && h_axis_tvalid;",
        replace="y_axis_tready = (state == S_IDLE);",
        killers=["sim:equalizer", "formal:equalizer"],
    ),
    # --- constellation_demapper --------------------------------------------
    dict(
        name="dm_top_cell_not_counted",
        file="ofdm/constellation_demapper.v",
        find="if (k == half) begin\n                        state <= S_U_B;",
        replace="if (k == half - 6'd1) begin\n                        state <= S_U_B;",
        killers=["sim:constellation_demapper"],
    ),
    dict(
        name="dm_gray_boundary_test_inverted",
        file="ofdm/constellation_demapper.v",
        find="wire        differ = gr1[gpos] != gr2[gpos];",
        replace="wire        differ = gr1[gpos] == gr2[gpos];",
        killers=["sim:constellation_demapper"],
    ),
    dict(
        name="dm_q_axis_llr_overwrites_i",
        file="ofdm/constellation_demapper.v",
        find="wr_idx  <= axis ? {1'b0, bpa} : 4'd0;",
        replace="wr_idx  <= 4'd0;",
        killers=["sim:constellation_demapper"],
    ),
    dict(
        name="dm_llr_truncates_instead_of_rounds",
        file="ofdm/constellation_demapper.v",
        find="(72'sd1 <<< (conv_sh - 6'd1))",
        replace="72'sd0",
        killers=["sim:constellation_demapper"],
    ),
    dict(
        name="dm_negative_clip_one_too_large",
        file="ofdm/constellation_demapper.v",
        find="(~{1'b0, clip_r} + 8'd1) :",
        replace="(~{1'b0, clip_r}) :",
        killers=["sim:constellation_demapper"],
    ),
    dict(
        name="dm_nuc2d_wrong_quadrant_sign",
        file="ofdm/constellation_demapper.v",
        find="(quad[0] ? -rom_re : rom_re);",
        replace="(quad[1] ? -rom_re : rom_re);",
        killers=["sim:constellation_demapper"],
    ),
    dict(
        name="dm_nuc4096_level_sign_bit",
        file="ofdm/constellation_demapper.v",
        find="wire        lvl_neg = is_4096 ? idx[5] : idx[4];",
        replace="wire        lvl_neg = idx[4];",
        killers=["sim:constellation_demapper"],
    ),
    dict(
        name="dm_nuc2d_bit_order_reversed",
        file="ofdm/constellation_demapper.v",
        find="if (idx[bps - 4'd1 - b[3:0]]) begin",
        replace="if (idx[b[3:0]]) begin",
        killers=["sim:constellation_demapper"],
    ),
    dict(
        name="dm_nuc1d_q_axis_bit_placement",
        file="ofdm/constellation_demapper.v",
        find="- {cb[2:0], 1'b0} - {3'd0, axis};",
        replace="- {cb[2:0], 1'b0};",
        killers=["sim:constellation_demapper"],
    ),
    dict(
        name="dm_tlast_on_every_beat",
        file="ofdm/constellation_demapper.v",
        find="m_axis_tlast  <= last_r && (out_idx + 4'd1 == bps);",
        replace="m_axis_tlast  <= last_r;",
        killers=["sim:constellation_demapper"],
    ),
    dict(
        name="dm_nuc_qpsk_point_value",
        file="ofdm/constellation_demapper.v",
        find="localparam [15:0] QPSK_Q15 = 16'd23170;",
        replace="localparam [15:0] QPSK_Q15 = 16'd23171;",
        killers=["sim:constellation_demapper"],
    ),
]
