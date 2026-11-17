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

- Branch `feature/hdl-port-foundations` rebased onto `develop`
  2026-10-29; still needs a rebase onto develop 9729ee4 (v1.0.1 merge).
- Spec gaps before 9.10 (2026-11-16/17, HISTORY.md), committed one per
  gap, not pushed: BCH, bit/frequency/CTI/HTI/cell de-interleavers, BBP,
  L1-Basic/L1-Detail, A/321 bootstrap, bootstrap-driven FrameSync,
  Table 8.9 guard intervals. Every commit builds and passes `ctest -L
  unit` float and fxp; tip float 757/757, fxp 760/760 unit+integration;
  cppcheck (CI flags) and clang-format clean.
- RTL done through 9.9 (demapper, equalizer, channel estimator). Stale
  against the golden model (benches fail visibly): `bootstrap_detector.v`,
  `frame_sync*.v` + `frame_correlator.v` (retired: FrameSync has no
  sample path; `frame_sync_gen` always fails), `cp_removal.v` (scales the
  GI by FFT size; mismatches at 16K/32K).

## Next

1. User: approve `docs/compliance.md` + README rewrite.
2. Remaining gaps: resampling (N210 rates to 6.144 MS/s bootstrap and
   (bsr+16) x 0.384 MS/s post-bootstrap, 6.912 for 6 MHz); receive-chain
   wiring (preamble FFT/GI per Annex H, preamble pilots, L1 cell
   extraction, NUC demap per L1 mode, PLP cells, GR blocks/GRC/bindings).
3. Then re-port bootstrap RTL, 9.10 de-interleavers, 9.11 LDPC, 9.12
   BCH, 9.13 L1 + config sequencer, 9.14 ALP, 9.15 AXI4-Lite, 9.16
   end-to-end, 9.17 throughput. Test bed: `hdl/docs/hg_xcku5p_testbed.md`.

## Open

- `cp_removal.v` fix needs a register-map change (approval): CP_LENGTH
  lacks GI12_4864 and is named as a fraction; `Atsc3Config::CpLength`
  is the same 11-value list. FrameSync handles time-aligned frame
  length only (symbol-aligned needs the L1 symbol layout).
- Not done in lib: CDL (inter-subframe HTI); nothing fills
  `Atsc3Config`; cell block r=0, time block CTI-only until the PLP chain;
  register map `c_source` cites removed `l1_decoder.h` (needs approval).
- Approval before commit: `docs/compliance.md` and README signal chain,
  L1 and module text (rewritten 2026-11-17).
- No off-air capture; `test/captures/` is synthetic (A/321 bootstraps in
  Gaussian fill). A real capture is the only end-to-end conformance test.
- `main`/`develop` do not configure on this host (Boost 1.90 has no
  `boost_system`); verified by direct compiles.
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
- Throughput (9.17): freq_correction 18 cycles/sample, FFT ~192.5k
  cycles per 8K symbol vs a ~16 cycles/sample budget at 100 MHz;
  `ofdm_frontend_tb` `realtime_budget_8k` is an expected failure.
- Formal harnesses other than pilot_extractor have not been re-timed
  with yices (qualified 2026-10-02; z3 stays their default).
- CI runs no HDL: `ci.yml` is ubuntu-22.04 (older Verilator/Yosys) and
  SymbiYosys 0.68 is a source install here. Needs an ubuntu-24.04 job
  (or the CI container) pinned to `hdl/docs/toolchain.md` versions.
- Before merging this branch: no `reuse lint` run yet (not installed).
- N210 offline and UHD images not downloaded (`uhd_images_downloader`);
  hardware tests skip. CI float build only.
- `config/hdl_register_map.json`: `angle_tbd` register format should
  adopt CORDIC's Q1.15 turns-over-pi when the phase tracker is ported.
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
