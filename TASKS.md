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

- Branch `feature/hdl-port-foundations` (local commits not yet pushed).
- `lib/`: float and fixed-point builds 655/655 ctest, including the 11
  capture-replay tests on the synthetic fixtures (committed 2026-10-01,
  LFS objects not yet pushed).
- Licensing: branch `feature/licensing` (worktree `../usrp-atsc3-licensing`,
  off `develop`, unpushed): NUC tables generated from A/322
  (`scripts/gen_nuc_tables.py`), built-in float FFT with FFTW preferred
  when found (`ATSC3_USE_FFTW=AUTO|ON|OFF`), MPL-2.0 / CERN-OHL-W-2.0
  (RTL) / CC-BY-4.0 (docs), SPDX headers, `LICENSING.md`, `REUSE.toml`.
- RTL done to the bar above: `axi4s_skid_buffer`, `udiv_seq`, `cordic`,
  `bootstrap_detector` (9.1), `polyphase_fir` + `timing_recovery` (9.2),
  `cp_removal` (9.3), `fft_engine` (9.4); integration bench
  `ofdm_frontend_tb` (cp_removal -> fft_engine).
- Gate: `hdl/run_all.sh [--mutants]` (lint, synth, formal, sim; 27
  mutants). `HDL_FULL=1` adds nightly sizes (32K FFT, 16K chain).

## Next

1. 9.5 Pilot extraction + frequency correction RTL: pilot ROM (PP1-PP8
   from `pilot_extractor.h`); pin SCATTERED > CONTINUAL > EDGE dedup in
   the golden model first; CORDIC NCO for `freq_correction`. Add both to
   the integration bench (after FFT).
2. 9.6 Frame sync RTL (after FFT + frequency correction; largest
   correlator; pin the golden CLI's sample chunking).
3. 9.7 Channel estimator, 9.8 equalizer, 9.9 constellation demapper,
   9.10 de-interleavers, 9.11 LDPC, 9.12 BCH, 9.13 L1 decode + config
   sequencer, 9.14 ALP demux, 9.15 AXI4-Lite control plane, 9.16
   end-to-end pipeline test, 9.17 throughput/timing closure. Scope notes
   per block: HISTORY.md archive, "Phase 9".

## Open

- Throughput (9.17): bootstrap detector ~150 cycles/sample, FFT ~192.5k
  cycles per 8K symbol; at 6.25 MS/s / 100 MHz the budget is 16
  cycles/sample (147k per 8K+CP symbol). `ofdm_frontend_tb`'s
  `realtime_budget_8k` is an expected failure until this closes (it
  measures 167,922 source-stall cycles over two 8K symbols).
- CI runs no HDL: `ci.yml` is ubuntu-22.04 (older Verilator/Yosys) and
  SymbiYosys 0.68 is a source install here. Needs an ubuntu-24.04 job
  (or the CI container) pinned to `hdl/docs/toolchain.md` versions.
- Capture fixtures: pushing them uploads ~100 MB of LFS objects
  (`lfs.github.com` reachable again 2026-10-01; not yet attempted).
- Licensing follow-up: After `feature/licensing`
  merges and this branch rebases: SPDX headers on this branch's new files
  (`hdl/rtl/**` CERN-OHL-W-2.0, rest MPL-2.0) and replace the
  nuc_tables license notes in `hdl/docs/placeholder_status.md` and
  `config/hdl_register_map.json`. No `reuse lint` yet (pip install stalled).
- `develop` itself: fixed-point test build fails (`test_metrics.cc`
  narrowing, fixed here in 747ec8e); fixed-point NUC/demapper tests (8)
  fail; capture tests fail on stale LFS pointers. All fixed on this branch.
- GNU Radio and UHD are not installed here: `blocks/` and the UHD source
  are never built or tested locally.
- `config/hdl_register_map.json`: `angle_tbd` register format should
  adopt CORDIC's Q1.15 turns-over-pi when the phase tracker is ported.
- `docs/compliance.md` is stale (calls the frequency deinterleaver
  "LFSR"; its pass counts predate the NUC tables and test fixes).
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
- Project name "gr-atsc3" is also the name of an existing GPL-3.0 GNU
  Radio module (drmpeg/gr-atsc3); consider renaming before release.
