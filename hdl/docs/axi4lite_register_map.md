# gr-atsc3 HDL AXI4-Lite Register Map

> Generated from `config/hdl_register_map.json` (v0.1.0) by `hdl/docs/generate_register_map.py`. Edit the JSON, not this file, and re-run the script.

Source of truth for the RTL control plane's default-reset values and host/L1-sequencer write ownership. Generated doc: hdl/docs/axi4lite_register_map.md via hdl/docs/generate_register_map.py. Phase 9.0 deliverable -- see the HDL port plan.

## Conventions

**Address layout:** One 0x1000-aligned region per block, in Phase 9.x order. Register offsets within a region are 4-byte aligned.

**Write ownership (`writable_by`):**

- `host` — Host-writable at any time via AXI4-Lite. Tuning/algorithm-select/test-override parameters not signaled by the ATSC 3.0 transmitter.
- `l1` — Read-only to the host. Written internally by the Phase 9.13b config sequencer after L1-Pre/L1-Post decode (or computed from those fields). Reset value is the standalone-test default (see l1_status block) so Phases 9.3-9.12 are independently testable before 9.13b exists.
- `const` — Not a real runtime register. Documents a compile-time/spec-fixed constant (tied off at synthesis). Included here for completeness only.

**Register formats:**

- `uint32` — Plain unsigned 32-bit integer (counts, sizes, whole-Hz values).
- `int32` — Plain signed 32-bit integer.
- `int8` — Plain signed 8-bit integer.
- `bool` — 1-bit register.
- `enum` — Integer register using the C++ enum's own explicit underlying values (see enum_values).
- `q1_15` — Signed Q1.15 fixed point, scale 32768, range [-1.0, +1.0), truncated toward zero (not rounded) on conversion -- matches lib/types.h float_to_q15()/q15_to_float() exactly (static_cast<int16_t>(x * 32768) truncates). All reset values below were computed with this exact truncation, not round-to-nearest. +1.0 must be represented as 32767, never 32768 (CLAUDE.md's documented convention) -- note that develop's float_to_q15() does not yet enforce this by saturating; that fix exists only on the unmerged bugfix/hdl-golden-model-fixes branch. No register in this map defaults to exactly 1.0, so the distinction doesn't affect any reset value here, but RTL bit-exactness work (Phase 9.0b onward) depends on that branch being merged first. Same truncation/scale convention as the ci16 datapath (hdl/docs/q_format_notes.md).
- `milli_fixed32` — Signed 32-bit integer, value = round(real_value * 1000). Used for dimensionless ratios/rates that can legitimately exceed the q1_15 [-1,1) range or need sub-integer-Hz precision.
- `angle_tbd` — Format intentionally left undefined. Depends on the CORDIC angle convention Phase 9.0b has not yet chosen (see hdl/docs/formal_conventions.md and the HDL port plan's Decision 7 / Phase 9.0b). Do not implement RTL against this field until that convention is fixed.

**Pilot pattern source:** Uses lib/ofdm/pilot_extractor.h's PP1-PP8 {Dx,Dy} table (config::PilotPattern / PilotPattern enum), not config/atsc3_modes.json's separate SP3_2/SP3_4/... representation -- resolving the Phase 9.0 open item on which of the two inconsistent pilot representations this register map follows. Nothing in lib/ reads atsc3_modes.json's SP*_* list (TASKS.md's claim that it's used is stale, matching pilot_extractor.cc's hardcoded constexpr table).

## Known default inconsistencies in the golden model

Found while cross-referencing every block's C++ config defaults against each other to build this register map. Not fixed here -- flagged as candidate `lib/` fixes for a future bugfix branch.

### `cp_length / cp_fraction at 8K`

Three C++ structs default this to two different values. lib/sync/frame_sync.h FrameSyncConfig::frame_params.cp_length = 512 (1/16, comment says '1/16 of 8K'). lib/ofdm/cp_removal.h CpRemovalConfig::cp_fraction = CpFraction::k1024_8192 (1024, 1/8) and lib/framing/l1_decoder.h L1Pre::cp_length = CpLength::CP_1024_8192 (also 1/8) agree with each other but not with FrameSyncConfig.

**Resolution used in this register map:** l1_status.cp_length below uses 1024 (1/8), matching CpRemovalConfig and L1Pre (2 of 3 sources, and the actual Phase 9.3 RTL target). FrameSyncConfig's frame_params default is the outlier -- flagged as a candidate lib/ fix, not corrected here (out of this deliverable's scope; not one of the original 7 bugs on bugfix/hdl-golden-model-fixes).

### `modulation (demapper vs. PLP)`

lib/ofdm/constellation_demapper.h DemapperConfig::modulation defaults to QAM64. lib/config/atsc3_config.h PlpConfig::modulation (the L1-signaled per-PLP value the demapper is actually supposed to run with) defaults to QPSK.

**Resolution used in this register map:** l1_status.modulation below uses QPSK, matching PlpConfig (the true L1-derived source). DemapperConfig's own QAM64 default only matters when the demapper is unit-tested standalone outside the L1 pipeline; flagged as a candidate lib/ fix, not corrected here.

### `symbol_rate_hz`

lib/sync/timing_recovery.h TimingRecoveryConfig::symbol_rate_hz defaults to a hardcoded 718.75 (comment: '~6.25e6/8704'). The exact value for the paired fft_size=8192/cp_length=512 defaults is 6.25e6/8704 = 718.1985..., about 0.08% off.

**Resolution used in this register map:** Minor; register uses the literal C++ default (718750 milli-Hz) rather than the exact recomputation, since that's what the golden model actually runs with.

## `l1_status` — canonical L1-decode mirror

Base address: `0x0000`

Canonical mirror of L1-Pre/L1-Post/active-PLP decode results (lib/framing/l1_decoder.h L1Pre, L1Post, and the single active entry of config::PlpConfig -- multi-PLP context switching is out of scope, see the HDL port plan). Every other block's l1-owned fields are wired internally from this bank, not independently stored -- there is exactly one copy of each L1-derived value in the register file. Written by the Phase 9.13b sequencer; reset values below are the standalone-test defaults used before 9.13b exists.

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `L1PRE_VERSION` | `0x00` | uint32 | `0` | l1 | *(`lib/framing/l1_decoder.h:L1Pre::version`)* |
| `FFT_SIZE` | `0x04` | enum | `FFT_8K` | l1 | *(`lib/framing/l1_decoder.h:L1Pre::fft_size (config::FftSize)`)* |
| `CP_LENGTH` | `0x08` | enum | `CP_1024_8192` | l1 | *(`lib/framing/l1_decoder.h:L1Pre::cp_length (config::CpLength); see known_default_inconsistencies`)* |
| `PILOT_PATTERN` | `0x0C` | enum | `PP3` | l1 | *(`lib/framing/l1_decoder.h:L1Pre::pilot_pattern (config::PilotPattern)`)* |
| `L1_POST_SIZE_CELLS` | `0x10` | uint32 | `0` | l1 | *(`lib/framing/l1_decoder.h:L1Pre::l1_post_size_cells`)* |
| `L1_POST_MODULATION` | `0x14` | enum | `QPSK` | l1 | *(`lib/framing/l1_decoder.h:L1Pre::l1_post_modulation (config::Modulation)`)* |
| `L1_POST_CODE_RATE` | `0x18` | enum | `RATE_5_15` | l1 | *(`lib/framing/l1_decoder.h:L1Pre::l1_post_code_rate (config::CodeRate)`)* |
| `L1_POST_SCRAMBLED` | `0x1C` | bool | `0` | l1 | *(`lib/framing/l1_decoder.h:L1Pre::l1_post_scrambled`)* |
| `L1_POST_FEC_TYPE` | `0x20` | enum | `BCH_16K_LDPC` | l1 | C++ field is a raw uint8_t, not an enum class -- the two named values here come from its doc comment ('0 = BCH + 16K LDPC, 1 = BCH + 64K LDPC'), not a C++ enum definition. *(`lib/framing/l1_decoder.h:L1Pre::l1_post_fec_type`)* |
| `NUM_SUBFRAMES` | `0x24` | uint32 | `1` | l1 | *(`lib/framing/l1_decoder.h:L1Pre::num_subframes`)* |
| `PREAMBLE_REDUCED_CARRIERS` | `0x28` | bool | `0` | l1 | *(`lib/framing/l1_decoder.h:L1Pre::preamble_reduced_carriers`)* |
| `L1PRE_CRC_VALID` | `0x2C` | bool | `0` | l1 | *(`lib/framing/l1_decoder.h:L1Pre::crc_valid`)* |
| `NUM_PLPS` | `0x30` | uint32 | `0` | l1 | *(`lib/framing/l1_decoder.h:L1PostConfigurable::num_plps`)* |
| `L1POST_TIME_INFO_PRESENT` | `0x34` | bool | `0` | l1 | *(`lib/framing/l1_decoder.h:L1PostConfigurable::time_info_present`)* |
| `FRAME_INDEX` | `0x38` | uint32 | `0` | l1 | *(`lib/framing/l1_decoder.h:L1Post::frame_index`)* |
| `L1_CHANGE_COUNTER` | `0x3C` | uint32 | `0` | l1 | *(`lib/framing/l1_decoder.h:L1Post::l1_change_counter`)* |
| `L1POST_CRC_VALID` | `0x40` | bool | `0` | l1 | *(`lib/framing/l1_decoder.h:L1Post::crc_valid`)* |
| `PLP_ID` | `0x44` | uint32 | `0` | l1 | *(`lib/config/atsc3_config.h:PlpConfig::plp_id (active PLP only; multi-PLP out of scope)`)* |
| `PLP_TYPE` | `0x48` | enum | `NON_DISPERSED` | l1 | *(`lib/config/atsc3_config.h:PlpConfig::plp_type`)* |
| `MODULATION` | `0x4C` | enum | `QPSK` | l1 | *(`lib/config/atsc3_config.h:PlpConfig::modulation; see known_default_inconsistencies`)* |
| `CODE_RATE` | `0x50` | enum | `RATE_7_15` | l1 | *(`lib/config/atsc3_config.h:PlpConfig::code_rate`)* |
| `CODEWORD_LENGTH_SHORT` | `0x54` | bool | `0` | l1 | *(`lib/config/atsc3_config.h:PlpConfig::codeword_length (LONG=64800/false, SHORT=16200/true)`)* |
| `TI_MODE` | `0x58` | enum | `CTI` | l1 | *(`lib/config/atsc3_config.h:PlpConfig::ti_mode`)* |
| `TI_DEPTH` | `0x5C` | uint32 | `0` | l1 | *(`lib/config/atsc3_config.h:PlpConfig::ti_depth (0-15)`)* |
| `TI_NUM_BLOCKS` | `0x60` | uint32 | `0` | l1 | *(`lib/config/atsc3_config.h:PlpConfig::ti_num_blocks`)* |
| `TI_EXTENDED` | `0x64` | bool | `0` | l1 | *(`lib/config/atsc3_config.h:PlpConfig::ti_extended`)* |
| `NUM_FEC_BLOCKS` | `0x68` | uint32 | `0` | l1 | *(`lib/config/atsc3_config.h:PlpConfig::num_fec_blocks`)* |
| `FEC_BLOCK_START` | `0x6C` | uint32 | `0` | l1 | *(`lib/config/atsc3_config.h:PlpConfig::fec_block_start`)* |
| `NUM_ACTIVE_CARRIERS` | `0x70` | uint32 | `6913` | l1 | *(`computed from FFT_SIZE (8K->6913, 16K->13825, 32K->27649), per lib/ofdm/pilot_extractor.cc / freq_deinterleaver.cc auto-compute logic`)* |

**Excluded fields** (present in the C++ config struct, deliberately not a register):

- `PlpConfig::plp_layer, PlpConfig::ldm_injection_level` — Layered Division Multiplexing (LDM) fields. The HDL port plan does not discuss LDM anywhere -- it's not listed as in-scope (no LDM combining/splitting stage appears in the 13-block signal chain) or explicitly out-of-scope. Left out of this register map as an open scope question for whoever picks up LDM, rather than silently including registers for a capability no RTL block implements.
- `PlpConfig::valid` — C++ object-model validity flag (whether this PlpConfig was ever populated). Not a hardware concept -- the l1_status bank's own CRC_VALID registers are the RTL-meaningful equivalent.

## Per-block registers

### `bootstrap_detector` (Phase 9.1)

Base address: `0x1000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `SAMPLE_RATE_HZ` | `0x00` | uint32 | `6250000` | host | System RF sample rate; not L1-signaled. *(`lib/sync/bootstrap_detector.h:BootstrapConfig::sample_rate_hz`)* |
| `THRESHOLD` | `0x04` | q1_15 | `22937` (0.7) | host | *(`lib/sync/bootstrap_detector.h:BootstrapConfig::threshold`)* |
| `AVERAGING_WINDOW` | `0x08` | uint32 | `64` | host | Must be >=1 (bugfix/hdl-golden-model-fixes clamps this in C++; RTL should reject 0 at the register-write boundary, see Phase 9.15 formal target). *(`lib/sync/bootstrap_detector.h:BootstrapConfig::averaging_window`)* |

### `timing_recovery` (Phase 9.2)

Base address: `0x2000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `SAMPLE_RATE_HZ` | `0x00` | uint32 | `6250000` | host | *(`lib/sync/timing_recovery.h:TimingRecoveryConfig::sample_rate_hz`)* |
| `SYMBOL_RATE_MILLIHZ` | `0x04` | milli_fixed32 | `718750` (718.75) | host | *(`lib/sync/timing_recovery.h:TimingRecoveryConfig::symbol_rate_hz; see known_default_inconsistencies`)* |
| `LOOP_BANDWIDTH_HZ` | `0x08` | uint32 | `1` | host | *(`lib/sync/timing_recovery.h:TimingRecoveryConfig::loop_bandwidth_hz`)* |
| `LOOP_DAMPING_MILLI` | `0x0C` | milli_fixed32 | `1000` (1.0) | host | *(`lib/sync/timing_recovery.h:TimingRecoveryConfig::loop_damping`)* |
| `POLYPHASE_NUM_PHASES` | `0x10` | uint32 | `16` | const | Fixes the polyphase bank RTL structure (16 banks) -- not runtime-reconfigurable, tied off at synthesis. *(`lib/sync/timing_recovery.h:PolyphaseConfig::num_phases`)* |
| `POLYPHASE_TAPS_PER_PHASE` | `0x14` | uint32 | `32` | const | Fixes the FIR tap-ROM depth -- synthesis-time constant. *(`lib/sync/timing_recovery.h:PolyphaseConfig::taps_per_phase`)* |
| `SAMPLES_PER_SYMBOL` | `0x18` | uint32 | `2` | host | *(`lib/sync/timing_recovery.h:PolyphaseConfig::samples_per_symbol`)* |
| `INITIAL_OFFSET` | `0x1C` | q1_15 | `0` (0.0) | host | *(`lib/sync/timing_recovery.h:TimingRecoveryConfig::initial_offset`)* |

### `cp_removal` (Phase 9.3)

Base address: `0x3000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `FFT_SIZE` | `0x00` | enum | `FFT_8K` | l1 | Wired from l1_status.FFT_SIZE, not independently stored. *(`lib/ofdm/cp_removal.h:CpRemovalConfig::fft_size`)* |
| `CP_LENGTH` | `0x04` | enum | `CP_1024_8192` | l1 | Wired from l1_status.CP_LENGTH, not independently stored. *(`lib/ofdm/cp_removal.h:CpRemovalConfig::cp_fraction (CpFraction, encoded as the literal sample count)`)* |

### `fft_engine` (Phase 9.4)

Base address: `0x4000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `FFT_SIZE` | `0x00` | enum | `k8K` | l1 | Wired from l1_status.FFT_SIZE for data symbols; bootstrap correlation always uses k4K regardless (CLAUDE.md Common Pitfalls) and is not register-controlled. *(`lib/ofdm/fft_engine.h:FftConfig::size`)* |
| `DIRECTION` | `0x04` | enum | `kForward` | const | Receiver is forward-FFT-only in RTL scope; inverse direction is a C++-only capability not ported (no transmit path in this receiver). *(`lib/ofdm/fft_engine.h:FftConfig::direction`)* |

**Excluded fields** (present in the C++ config struct, deliberately not a register):

- `wisdom_path` — FFTW-specific plan-caching path; C++-only, no RTL equivalent (RTL's memory-based radix-2 DIT has no planning phase, Decision 2).
- `use_patient_plan` — Same as wisdom_path -- FFTW planning knob, not applicable to RTL.
- `normalize_inverse` — Only meaningful for inverse FFT, which is out of RTL scope (see DIRECTION above).

### `pilot_extractor` (Phase 9.5)

Base address: `0x5000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `FFT_SIZE` | `0x00` | enum | `FFT_8K` | l1 | Wired from l1_status.FFT_SIZE. *(`lib/ofdm/pilot_extractor.h:PilotExtractorConfig::fft_size`)* |
| `PILOT_PATTERN` | `0x04` | enum | `PP3` | l1 | Wired from l1_status.PILOT_PATTERN. *(`lib/ofdm/pilot_extractor.h:PilotExtractorConfig::pattern`)* |
| `NUM_ACTIVE_CARRIERS` | `0x08` | uint32 | `6913` | l1 | Wired from l1_status.NUM_ACTIVE_CARRIERS. *(`lib/ofdm/pilot_extractor.h:PilotExtractorConfig::num_active_carriers`)* |

**Open item:** Pilot dedup precedence (SCATTERED > CONTINUAL > EDGE) needs an explicit rule fixed in lib/ before this block's RTL is bit-exact (Phase 9.0 item from the plan's pass-2 review); not resolved by this register-map deliverable.

### `freq_correction` (Phase 9.5)

Base address: `0x6000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `SAMPLE_RATE_HZ` | `0x00` | uint32 | `6250000` | host | *(`lib/sync/freq_correction.h:FreqCorrectionConfig::sample_rate_hz`)* |
| `INITIAL_CFO_HZ` | `0x04` | int32 | `0` | host | Seeded from bootstrap_detector's CFO estimate at acquisition; host-writable for standalone test. *(`lib/sync/freq_correction.h:FreqCorrectionConfig::initial_cfo_hz`)* |
| `ENABLE_FINE_TRACKING` | `0x08` | bool | `1` | host | *(`lib/sync/freq_correction.h:FreqCorrectionConfig::enable_fine_tracking`)* |
| `FINE_LOOP_BANDWIDTH_HZ` | `0x0C` | uint32 | `10` | host | *(`lib/sync/freq_correction.h:FreqCorrectionConfig::fine_loop_bandwidth_hz`)* |
| `PHASE_TRACKER_FFT_SIZE` | `0x10` | enum | `FFT_8K` | l1 | Wired from l1_status.FFT_SIZE. *(`lib/sync/freq_correction.h:PilotPhaseTrackerConfig::fft_size`)* |
| `PHASE_TRACKER_AVERAGING_WINDOW` | `0x14` | uint32 | `4` | host | *(`lib/sync/freq_correction.h:PilotPhaseTrackerConfig::averaging_window`)* |
| `PHASE_TRACKER_UNWRAP_THRESHOLD` | `0x18` | angle_tbd | *(TBD)* | host | Default is exactly M_PI, which is why this can't use q1_15 (out of [-1,1) range) -- needs the Phase 9.0b CORDIC angle convention before this register's format/reset can be finalized. *(`lib/sync/freq_correction.h:PilotPhaseTrackerConfig::unwrap_threshold`)* |

### `frame_sync` (Phase 9.6)

Base address: `0x7000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `SAMPLE_RATE_HZ` | `0x00` | uint32 | `6250000` | host | *(`lib/sync/frame_sync.h:FrameSyncConfig::sample_rate_hz`)* |
| `FFT_SIZE` | `0x04` | uint32 | `8192` | l1 | Wired from l1_status.FFT_SIZE. *(`lib/sync/frame_sync.h:FrameSyncConfig::frame_params.fft_size`)* |
| `CP_LENGTH` | `0x08` | uint32 | `1024` | l1 | Wired from l1_status.CP_LENGTH (1024); see known_default_inconsistencies for why this isn't FrameParams's own literal default of 512. *(`lib/sync/frame_sync.h:FrameSyncConfig::frame_params.cp_length`)* |
| `SYMBOLS_PER_SUBFRAME` | `0x0C` | uint32 | `1` | l1 | *(`lib/sync/frame_sync.h:FrameSyncConfig::frame_params.symbols_per_subframe`)* |
| `SUBFRAMES_PER_FRAME` | `0x10` | uint32 | `1` | l1 | Wired from l1_status.NUM_SUBFRAMES. *(`lib/sync/frame_sync.h:FrameSyncConfig::frame_params.subframes_per_frame`)* |
| `PREAMBLE_SYMBOLS` | `0x14` | uint32 | `1` | l1 | *(`lib/sync/frame_sync.h:FrameSyncConfig::frame_params.preamble_symbols`)* |
| `DETECTION_THRESHOLD` | `0x18` | q1_15 | `22937` (0.7) | host | *(`lib/sync/frame_sync.h:FrameSyncConfig::detection_threshold`)* |
| `LOCK_ACQUIRE_COUNT` | `0x1C` | uint32 | `3` | host | *(`lib/sync/frame_sync.h:FrameSyncConfig::lock_acquire_count`)* |
| `LOCK_LOSS_COUNT` | `0x20` | uint32 | `5` | host | *(`lib/sync/frame_sync.h:FrameSyncConfig::lock_loss_count`)* |
| `HOLDOVER_FRAMES` | `0x24` | uint32 | `10` | host | *(`lib/sync/frame_sync.h:FrameSyncConfig::holdover_frames`)* |
| `SEARCH_WINDOW` | `0x28` | uint32 | `64` | host | Directly sizes the acquisition correlator's search width -- see the HDL port plan's Phase 9.6 cycle-budget note (~2M complex MACs/call at default width, largest correlator in the design). *(`lib/sync/frame_sync.h:FrameSyncConfig::search_window`)* |

### `channel_estimator` (Phase 9.7)

Base address: `0x8000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `FFT_SIZE` | `0x00` | enum | `FFT_8K` | l1 | Wired from l1_status.FFT_SIZE. *(`lib/channel/channel_estimator.h:ChannelEstimatorConfig::fft_size`)* |
| `NUM_ACTIVE_CARRIERS` | `0x04` | uint32 | `6913` | l1 | Wired from l1_status.NUM_ACTIVE_CARRIERS. *(`lib/channel/channel_estimator.h:ChannelEstimatorConfig::num_active_carriers`)* |
| `BACKEND` | `0x08` | enum | `LS_ONLY` | host | ML_ONNX is a post-MVP stub, out of RTL scope -- writing this value is host-legal but has no RTL implementation to select. *(`lib/channel/channel_estimator.h:ChannelEstimatorConfig::backend`)* |
| `INTERPOLATION` | `0x0C` | enum | `LINEAR` | host | *(`lib/channel/channel_estimator.h:ChannelEstimatorConfig::interpolation`)* |
| `NOISE_VARIANCE` | `0x10` | q1_15 | `0` (0.0) | host | *(`lib/channel/channel_estimator.h:ChannelEstimatorConfig::noise_variance`)* |
| `ENABLE_AVERAGING` | `0x14` | bool | `0` | host | *(`lib/channel/channel_estimator.h:ChannelEstimatorConfig::enable_averaging`)* |
| `AVERAGING_ALPHA` | `0x18` | q1_15 | `3276` (0.1) | host | *(`lib/channel/channel_estimator.h:ChannelEstimatorConfig::averaging_alpha`)* |

### `wiener_interpolator` (Phase 9.7)

Base address: `0x9000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `FFT_SIZE` | `0x00` | enum | `FFT_8K` | l1 | *(`lib/channel/wiener_interpolator.h:WienerInterpolatorConfig::fft_size`)* |
| `NUM_ACTIVE_CARRIERS` | `0x04` | uint32 | `6913` | l1 | *(`lib/channel/wiener_interpolator.h:WienerInterpolatorConfig::num_active_carriers`)* |
| `FREQ_TAPS` | `0x08` | uint32 | `8` | const | Sizes the FIR tap-ROM -- synthesis-time constant. *(`lib/channel/wiener_interpolator.h:WienerInterpolatorConfig::freq_taps`)* |
| `TIME_TAPS` | `0x0C` | uint32 | `4` | const | Synthesis-time constant, same reasoning as FREQ_TAPS. *(`lib/channel/wiener_interpolator.h:WienerInterpolatorConfig::time_taps`)* |
| `PROFILE` | `0x10` | enum | `PEDESTRIAN` | host | Selects which precomputed Wiener filter coefficient ROM to use. *(`lib/channel/wiener_interpolator.h:WienerInterpolatorConfig::profile`)* |
| `NOISE_VARIANCE` | `0x14` | q1_15 | `327` (0.01) | host | *(`lib/channel/wiener_interpolator.h:WienerInterpolatorConfig::noise_variance`)* |
| `PILOT_SPACING_FREQ` | `0x18` | uint32 | `6` | l1 | Dx; wired from l1_status.PILOT_PATTERN's {Dx,Dy} (PP3 default matches Dx=6). *(`lib/channel/wiener_interpolator.h:WienerInterpolatorConfig::pilot_spacing_freq`)* |
| `PILOT_SPACING_TIME` | `0x1C` | uint32 | `4` | l1 | Dy; wired from l1_status.PILOT_PATTERN (PP3 default matches Dy=4). *(`lib/channel/wiener_interpolator.h:WienerInterpolatorConfig::pilot_spacing_time`)* |

### `equalizer` (Phase 9.8)

Base address: `0xA000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `FFT_SIZE` | `0x00` | enum | `FFT_8K` | l1 | *(`lib/channel/equalizer.h:EqualizerConfig::fft_size`)* |
| `NUM_ACTIVE_CARRIERS` | `0x04` | uint32 | `6913` | l1 | *(`lib/channel/equalizer.h:EqualizerConfig::num_active_carriers`)* |
| `MODE` | `0x08` | enum | `ZERO_FORCING` | host | ZF and MMSE need separate eq_complex_divider.v paths per the HDL port plan (Phase 9.8) -- do not reuse the channel estimator's divider. *(`lib/channel/equalizer.h:EqualizerConfig::mode`)* |
| `NOISE_VARIANCE` | `0x0C` | q1_15 | `327` (0.01) | host | *(`lib/channel/equalizer.h:EqualizerConfig::noise_variance`)* |
| `ENABLE_PHASE_TRACKING` | `0x10` | bool | `1` | host | Default true -- the phase-tracking clamp-after-overflow bug fixed on bugfix/hdl-golden-model-fixes is live at this default, not an edge case. *(`lib/channel/equalizer.h:EqualizerConfig::enable_phase_tracking`)* |
| `PHASE_TRACKING_ALPHA` | `0x14` | q1_15 | `9830` (0.3) | host | *(`lib/channel/equalizer.h:EqualizerConfig::phase_tracking_alpha`)* |
| `MIN_CHANNEL_MAGNITUDE` | `0x18` | q1_15 | `327` (0.01) | host | This config value alone does not set the RTL deep-fade floor -- equalize_mmse always applies its own structural kMinMagnitudeSqDefault (fixed at 1<<15 post-bugfix) regardless of this register; see the HDL port plan's Phase 9.8 formal target ('the fallback divisor path is never taken'). *(`lib/channel/equalizer.h:EqualizerConfig::min_channel_magnitude`)* |

### `constellation_demapper` (Phase 9.9)

Base address: `0xB000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `MODULATION` | `0x00` | enum | `QPSK` | l1 | Wired from l1_status.MODULATION (QPSK) -- not DemapperConfig's own standalone-unit-test default of QAM64. *(`lib/ofdm/constellation_demapper.h:DemapperConfig::modulation; see known_default_inconsistencies`)* |
| `CODE_RATE` | `0x04` | enum | `RATE_7_15` | l1 | Wired from l1_status.CODE_RATE. Needed for NUC table selection (see hdl/docs/placeholder_status.md re: nuc_tables.h license check). *(`lib/ofdm/constellation_demapper.h:DemapperConfig::code_rate`)* |
| `NOISE_VARIANCE` | `0x08` | q1_15 | `3276` (0.1) | host | *(`lib/ofdm/constellation_demapper.h:DemapperConfig::noise_variance`)* |
| `USE_MAX_LOG` | `0x0C` | bool | `1` | const | Per the HDL port plan's Phase 9.0b, the generalized boundary-slicer (compute_1d_llr) becomes the one fixed-point path for every mode -- the exact-LLR (non-max-log) alternative is not planned for RTL, so this is effectively tied to true rather than a real runtime choice. *(`lib/ofdm/constellation_demapper.h:DemapperConfig::use_max_log`)* |
| `LLR_CLIP` | `0x10` | int8 | `127` | host | *(`lib/ofdm/constellation_demapper.h:DemapperConfig::llr_clip`)* |

### `cell_deinterleaver` (Phase 9.10)

Base address: `0xC000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `NUM_CELLS` | `0x00` | uint32 | `0` | l1 | Derived, not directly signaled: varies continuously with modulation x code rate x FEC block count. See the HDL port plan's Phase 9.10 note -- NOT simply ROM-able like the frequency deinterleaver; needs an on-chip permutation-builder FSM or an enumerated num_cells restriction (open design question, not resolved by this register-map deliverable). *(`lib/ofdm/cell_deinterleaver.h:CellDeinterleaverConfig::num_cells`)* |

### `time_deinterleaver` (Phase 9.10)

Base address: `0xD000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `MODE` | `0x00` | enum | `CTI` | l1 | Wired from l1_status.TI_MODE. *(`lib/ofdm/time_deinterleaver.h:TimeDeinterleaverConfig::mode`)* |
| `DEPTH` | `0x04` | uint32 | `0` | l1 | Wired from l1_status.TI_DEPTH. Directly drives the delay-line RAM budget -- see the HDL port plan's Phase 9.10 note (up to ~31 Mbit at depth=15/QPSK). *(`lib/ofdm/time_deinterleaver.h:TimeDeinterleaverConfig::depth`)* |
| `NUM_TI_BLOCKS` | `0x08` | uint32 | `0` | l1 | Wired from l1_status.TI_NUM_BLOCKS. *(`lib/ofdm/time_deinterleaver.h:TimeDeinterleaverConfig::num_ti_blocks`)* |
| `CELLS_PER_BLOCK` | `0x0C` | uint32 | `0` | l1 | Derived from NUM_FEC_BLOCKS and the modulation/code-rate cells-per-FEC-block, not itself a literal L1 bitstream field. *(`lib/ofdm/time_deinterleaver.h:TimeDeinterleaverConfig::cells_per_block`)* |

### `freq_deinterleaver` (Phase 9.10)

Base address: `0xE000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `FFT_SIZE` | `0x00` | enum | `FFT_8K` | l1 | Wired from l1_status.FFT_SIZE. Selects one of exactly 3 precomputed permutation ROMs (n in {6913,13825,27649}) -- see the HDL port plan's Phase 9.10 note. *(`lib/ofdm/freq_deinterleaver.h:FreqDeinterleaverConfig::fft_size`)* |
| `NUM_ACTIVE_CARRIERS` | `0x04` | uint32 | `6913` | l1 | Wired from l1_status.NUM_ACTIVE_CARRIERS. *(`lib/ofdm/freq_deinterleaver.h:FreqDeinterleaverConfig::num_active_carriers`)* |

### `ldpc_decoder` (Phase 9.11)

Base address: `0xF000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `CODE_RATE` | `0x00` | enum | `RATE_7_15` | l1 | Wired from l1_status.CODE_RATE. Selects the per-rate sparse H-matrix ROM (non-spec placeholder matrices, see hdl/docs/placeholder_status.md). *(`lib/fec/ldpc_decoder.h:LdpcConfig::code_rate`)* |
| `SHORT_CODEWORD` | `0x04` | bool | `0` | l1 | Wired from l1_status.CODEWORD_LENGTH_SHORT. *(`lib/fec/ldpc_decoder.h:LdpcConfig::short_codeword`)* |
| `MAX_ITERATIONS` | `0x08` | uint32 | `50` | host | *(`lib/fec/ldpc_decoder.h:LdpcConfig::max_iterations`)* |
| `EARLY_TERM_THRESHOLD` | `0x0C` | q1_15 | `0` (0.0) | host | 0 = disabled. *(`lib/fec/ldpc_decoder.h:LdpcConfig::early_term_threshold`)* |
| `MIN_SUM_SCALE` | `0x10` | q1_15 | `24576` (0.75) | host | *(`lib/fec/ldpc_decoder.h:LdpcConfig::min_sum_scale`)* |

**Excluded fields** (present in the C++ config struct, deliberately not a register):

- `use_fixed_point` — C++-only build-mode switch selecting between the float and int16 decoder paths; RTL is inherently the fixed-point path, so this doesn't apply.

### `bch_decoder` (Phase 9.12)

Base address: `0x10000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `CODEWORD_LENGTH` | `0x00` | uint32 | `57600` | l1 | Varies with the selected LDPC frame size/code rate (l1_status.CODEWORD_LENGTH_SHORT and CODE_RATE). *(`lib/fec/bch_decoder.h:BchConfig::codeword_length`)* |
| `INFO_LENGTH` | `0x04` | uint32 | `57408` | l1 | *(`lib/fec/bch_decoder.h:BchConfig::info_length`)* |
| `T` | `0x08` | uint32 | `12` | const | ATSC A/322-fixed BCH error-correction capability -- not a runtime choice. *(`lib/fec/bch_decoder.h:BchConfig::t`)* |
| `M` | `0x0C` | uint32 | `16` | const | GF(2^16) -- spec-fixed, sizes the gf_exp_/gf_log_ ROMs. *(`lib/fec/bch_decoder.h:BchConfig::m`)* |

### `l1_decoder` (Phase 9.13a)

Base address: `0x11000`

Control registers only -- decoded L1 field values themselves live in the top-level l1_status bank, not duplicated here.

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `MAX_LDPC_ITERATIONS` | `0x00` | uint32 | `25` | host | L1 decode itself is a bare LLR hard-decision slicer today (no real LDPC/BCH), see hdl/docs/placeholder_status.md -- this register is forward-looking for when that's fixed. *(`lib/framing/l1_decoder.h:L1DecoderConfig::max_ldpc_iterations`)* |
| `CHECK_CRC` | `0x04` | bool | `1` | host | Test/debug override. *(`lib/framing/l1_decoder.h:L1DecoderConfig::check_crc`)* |
| `L1_PRE_SHORT_LDPC` | `0x08` | bool | `1` | const | L1-Pre always uses the short (16200) LDPC codeword per spec -- not a runtime choice. *(`lib/framing/l1_decoder.h:L1DecoderConfig::l1_pre_short_ldpc`)* |

### `alp_demux` (Phase 9.14)

Base address: `0x12000`

| Register | Offset | Format | Reset | Writable by | Notes |
|---|---|---|---|---|---|
| `MAX_DATAGRAM_SIZE` | `0x00` | uint32 | `65535` | host | Can only reduce, never exceed, the synthesis-time input_buffer_ depth bound (the Phase 9.14 rule-5 fix -- see the HDL port plan; the unbounded resize() in the golden model is the actual defect being fixed, not ported). *(`lib/framing/alp_demux.h:AlpDemuxConfig::max_datagram_size`)* |
| `CHECK_CRC` | `0x04` | bool | `1` | host | Test/debug override. *(`lib/framing/alp_demux.h:AlpDemuxConfig::check_crc`)* |

**Excluded fields** (present in the C++ config struct, deliberately not a register):

- `reassembly_timeout_packets` — check_reassembly_timeouts() is unimplemented (no-op) in the golden model -- see hdl/docs/placeholder_status.md. No RTL timeout mechanism is built to match a reference that doesn't exist.
- `MAX_REASSEMBLY_CONTEXTS` — static constexpr = 16, already a compile-time array bound (std::array<ReassemblyContext,16>) in the golden model -- synthesis-time constant, not a register.
- `MAX_ALP_PACKET_SIZE` — static constexpr = 65535 -- synthesis-time constant, not a register.

## Explicitly out of scope

Config structs found in lib/ that are not part of this register map, per the HDL port plan's 'Explicitly out of scope' section.

- `lib/metrics/*.h` — SNR/MER/BER/RSSI metrics status registers -- out of scope this milestone (log10-based, no CORDIC hyperbolic-mode support planned yet).
- `lib/framing/route_parser.h` — ROUTE/DASH is host software, downstream of the ALP demux RTL output boundary.
- `lib/config/mode_config.h` — Flagged in the HDL port plan as unaudited surface worth a pass before its dependent phases start; not yet reviewed for this register map.

