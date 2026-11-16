# TASKS.md — gr-atsc3 status

> Current status only. History, decisions behind finished work and the
> full plan archive: `HISTORY.md`. Spec: `README.md`.

## Decision

- RTL ports the `ATSC3_FIXED_POINT=ON` golden model bit-exactly
  (IEEE 1364-2001, no vendor primitives); IQ in, ALP demux out.
- Per-block "done": interface contract (holds, latency, reset); unit
  cocotb bench on Verilator and Icarus vs. a golden CLI linking
  `atsc3_lib`; committed mutants; formal with a ghost-model integrity
  property (or a recorded reason); lint; synthesis; integration scenario.
- Formal: flatten+`expose` (not `bind`), `smtbmc z3`; ABC PDR via
  `prove_pdr.sh` for heavy datapaths; mutants via `hdl/mutants/`.
- Datapaths are sequential, correctness first; throughput is plan 9.17.

## Reason

A bit-exact reference turns every RTL mismatch into a defect, and the
two-simulator + mutant + ghost-model bar is what caught this branch's
legality errors, races and weak tests (see HISTORY.md, 2026-10-01).

## Status

- Branch `feature/hdl-port-foundations` is rebased onto `develop` and
  force-pushed (2026-10-29; backup: local `backup/pre-rebase`).
- Spec review before 9.10 (2026-11-16, uncommitted): the golden model
  is not an ATSC 3.0 receiver yet. LDPC fixed (Annex A tables, spec
  encoder, all 24 codes; float min-sum APP clamp + sign(0) bugs fixed).
  Still non-spec, `placeholder_status.md` #2, #6, #7, #11-#16: L1
  (DVB-T2 layout, no FEC), BCH (wrong field/lengths), bootstrap (4096
  repeated halves, not A/321), no bit de-interleaver, no BBP header or
  descrambler, wrong de-interleaver order/framing, no resampling.
  ctest float 669/669, fxp 676/676; cppcheck (CI flags) clean.
- RTL done through 9.9 (2026-11-16): demapper (11 mutants, formal
  0/6/12 only, FORMAL_TIMEOUT=4000); 9.8 equalizer; 9.7 channel
  estimator (`hdl/rtl/channel/`). Not ported: Wiener, phase tracking
  (#9, #10). Details in HISTORY.md.
- Earlier closures (FrameSync window, A/322 pilots, SPDX, gate sizes,
  GNU Radio 3.10.12 + UHD 3.15 in `/opt/uhd`, Ubuntu 26.04 host): HISTORY.md.

## Next

1. Test bed: `hdl/docs/hg_xcku5p_testbed.md` (plan only; hardware
   firmware deferred by user; schematics obtained; Vivado in /opt/Xilinx
   usable for synth/resource estimates).
2. User decision: re-sequence the plan around the spec gaps (BCH, bit
   de-interleaver, A/321 bootstrap, L1-Basic/Detail chain, real
   frequency/cell/time de-interleavers, BBP) before more RTL, since RTL
   is bit-exact to the golden model. Then 9.10 de-interleavers, 9.11
   LDPC, 9.12 BCH, 9.13 L1 decode + config sequencer, 9.14 ALP demux,
   9.15 AXI4-Lite, 9.16 end-to-end, 9.17 throughput. Scope notes per
   block: HISTORY.md archive, "Phase 9".

## Open

- The equalizer needs the Q30 registers (`cfg_min_mag_sq_q30`,
  `cfg_noise_q30`) software derives; the register map still lists
  NOISE_VARIANCE / MIN_CHANNEL_MAGNITUDE as Q1.15 (changing it needs
  approval; decide at the control-plane block, 9.15).
- No production block yet selects the active window from the FFT bins or
  buffers them until the estimate is ready (the bench uses a behavioural
  buffer): integration work (9.16).
- linear_interp formal needs ~21 min (yices) and a 4000 s limit; its
  proof is shrunk (6 carriers, 4-bit divider, always-ready handshake);
  3+ pilot lists, stalls and the flags are simulation-only.
- frame_sync throughput: one correlation is fft_size + ~170 cycles and a
  chunk runs up to search_window+1 of them with the input held off
  (~0.5M cycles at 8K/64); the bench measures +8,333 source-stall cycles
  per symbol (one correlation). Overlap/double-buffering is 9.17 work.
- frame_sync_fsm formal is bounded (BMC depth 30, cover depth 60), not
  unbounded; blends/products are cut. The correlator's arithmetic is
  covered by bit-exact simulation and mutants only.
- Throughput (9.17): bootstrap detector ~150 cycles/sample,
  freq_correction 18 cycles/sample (one iterative CORDIC per sample),
  FFT ~192.5k cycles per 8K symbol; at 6.25 MS/s / 100 MHz the budget is
  16 cycles/sample (147k per 8K+CP symbol). `ofdm_frontend_tb`'s
  `realtime_budget_8k` is an expected failure until this closes (it
  measures 514,051 source-stall cycles over two 8K symbols).
- No off-air ATSC 3.0 capture exists; `test/captures/` is synthetic and
  shaped to the non-spec bootstrap model. A real capture (or an A/322
  reference transmitter output) is the only end-to-end conformance test.
- `CLAUDE.md` ("bootstrap is always 4K OFDM") and README text describing
  L1-Pre/L1-Post contradict A/321/A/322; edits need user approval.
- Formal harnesses other than pilot_extractor have not been re-timed
  with yices (qualified 2026-10-02; z3 stays their default).
- CI runs no HDL: `ci.yml` is ubuntu-22.04 (older Verilator/Yosys) and
  SymbiYosys 0.68 is a source install here. Needs an ubuntu-24.04 job
  (or the CI container) pinned to `hdl/docs/toolchain.md` versions.
- Before merging this branch: no `reuse lint` run yet (not installed).
- `develop` itself: fixed-point test build fails (`test_metrics.cc`
  narrowing, fixed here in 747ec8e); fixed-point NUC/demapper tests (8)
  fail; capture tests fail on stale LFS pointers. All fixed on this branch.
- N210 offline and UHD images not downloaded (`uhd_images_downloader`);
  hardware tests skip. CI float build only.
- `config/hdl_register_map.json`: `angle_tbd` register format should
  adopt CORDIC's Q1.15 turns-over-pi when the phase tracker is ported.
- `docs/compliance.md` pass counts predate the NUC tables and test fixes.
- From plan 0-8: branch protection (manual GitHub setting); LDPC min-sum
  vectorization partial; end-to-end playback from a real capture never
  run.
- ML multipath mitigation milestone not started (HISTORY.md archive).
- Dependency licenses (copyleft *): GNU Radio* GPL-3.0, UHD* GPL-3.0,
  FFTW3* GPL-2.0+ (optional), FFmpeg* LGPL-2.1+, GStreamer* LGPL-2.1+,
  Boost BSL-1.0, GoogleTest/pybind11 BSD-3-Clause, ONNX Runtime MIT;
  dev tools: pypdf BSD-3-Clause (NUC generator), cocotb BSD-3-Clause,
  pytest MIT, Verilator LGPL-3.0/Artistic-2.0, Icarus GPL-2.0*, Yosys/SBY
  ISC, z3 MIT, cvc5 BSD-3-Clause. All compatible with MPL-2.0 as used.
