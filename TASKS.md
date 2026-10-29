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

- Branch `feature/hdl-port-foundations` rebased onto `develop` 2026-10-29
  (backup: local `backup/pre-rebase`); remote still has the old history, a
  force-push of the feature branch is pending approval.
- `lib/`: float and fixed-point builds 656/656 ctest, including the 11
  capture-replay tests on the synthetic fixtures. cppcheck (CI flags)
  clean locally.
- Closed 2026-10-22: FrameSync ACQUIRING window fix, GCC 15 alp_demux,
  cppcheck returnByReference, pilot model+RTL A/322-exact. Closed
  2026-10-29: scattered pilot patterns numbered by A/322 codes in config,
  L1 decoder (3-bit field, index 7 reserved), mode table, GR blocks, GRC;
  SPDX headers on the HDL tree; gate sizes re-baselined
  (`hdl/docs/synth_baseline.md`); CI builds fixed-point and runs the HDL
  gate (unrun: needs GitHub). GNU Radio 3.10.12 and UHD 3.15 (`/opt/uhd`,
  `docs/uhd_build.md`) installed: blocks, bindings and UHD source build,
  float ctest 662 (9 hardware skips), fxp 654.
- RTL done through 9.6 (2026-10-18): frame_sync (chunk buffer + FSM +
  correlator + isqrt_seq) meets the done bar; the front-end bench now
  taps the corrected stream into it. Golden change: the fixed-point
  state machine runs on Q1.15 integer scores/thresholds/confidence
  (float build unchanged). Host moved to Ubuntu 26.04 (Verilator 5.032,
  Yosys 0.52, Python 3.14, GCC 15, Boost 1.90): lint/synth scripts and
  the venv adapted, Boost.System made header-only; see
  `hdl/docs/toolchain.md`.
- Earlier, through 9.5 (2026-10-02): pilot_extractor and freq_correction
  meet the done bar; integration bench is now freq_correction ->
  cp_removal -> fft_engine -> pilot_extractor. Golden fix: pilot dedup
  precedence pinned (SCATTERED > CONTINUAL > EDGE; `std::sort` let an
  EDGE record win). Formal for pilot_extractor uses yices (z3 and PDR
  could not close it); yices 2.6.5 qualified.
- Licensing and rename merged to `develop` (PR #1, CI green
  2026-10-01): project is now OpenATSC3; MPL-2.0 / CERN-OHL-W-2.0 (RTL) /
  CC-BY-4.0 (docs); NUC tables generated from A/322; FFTW preferred when
  found (`ATSC3_USE_FFTW=AUTO|ON|OFF`) with a built-in fallback FFT.

## Next

1. Test bed: `hdl/docs/hg_xcku5p_testbed.md` (plan only; hardware
   firmware deferred by user; schematics obtained; Vivado in /opt/Xilinx
   usable for synth/resource estimates).
2. 9.7 Channel estimator, 9.8 equalizer, 9.9 constellation demapper,
   9.10 de-interleavers, 9.11 LDPC, 9.12 BCH, 9.13 L1 decode + config
   sequencer, 9.14 ALP demux, 9.15 AXI4-Lite control plane, 9.16
   end-to-end pipeline test, 9.17 throughput/timing closure. Scope notes
   per block: HISTORY.md archive, "Phase 9".

## Open

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
- The L1-Pre pilot pattern field is still a 3-bit index into seven
  patterns (not the A/331 field); replace it with the 9.13 L1 decode.
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
