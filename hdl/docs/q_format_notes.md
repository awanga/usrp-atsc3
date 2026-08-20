# Q-Format and Input Backoff Contract

> Phase 9.0 deliverable (see the HDL port plan, Decision 6). This is the
> explicit statement of what "full scale" means at the RTL IQ input, since
> AGC (`hal/src/agc.cc`) is excluded from RTL scope and its output is
> nowhere else specified for hardware consumers.

## Why this document exists

`AgcController::process()` (`hal/include/agc.h`, `hal/src/agc.cc`) only
*computes* a gain value; nothing in `hal/` applies that gain digitally to
the IQ stream. The gain is applied by `IQSource::set_gain()` calling into
UHD/hardware, upstream of everything RTL will ever see. That makes AGC
correctly out of RTL scope (per Decision 6) -- but it also means the RTL
input contract inherits an assumption from `AgcController` that is never
written down anywhere a hardware consumer would read it. This document is
that missing contract.

Several defects found during the golden-model review (equalizer phase
tracking, frequency-correction NCO output, both fixed in
`bugfix/hdl-golden-model-fixes`) only manifested near full-scale input.
Whether those failure modes are "always reachable in normal operation" or
"reachable only out of spec" depends entirely on the answer below.

## The contract

**RTL IQ input is Q1.15, and normal-operating-range input power is
-20 dBFS ± peak headroom, not full scale.**

- Container: `std::complex<int16_t>`, both rails Q1.15 (`[-32768, 32767]`,
  see `lib/types.h`; `+1.0` is represented as `32767`, never `32768`).
- `AgcController`'s default `target_power_dbfs = -20.0` (`hal/include/agc.h:18`)
  is the reference operating point: 20 dB of headroom below full scale is
  budgeted for OFDM peak-to-average power ratio (PAPR), which for ATSC 3.0
  can exceed 10 dB before clipping. **RTL blocks should be designed and
  tested around this operating point, not around full-scale input.**
- Full-scale input (`|re|, |im|` at or near 32767) is a valid *transient*
  condition (a PAPR peak) that every block must saturate cleanly on, not
  a sustained operating condition. A block that only behaves correctly
  when the average input power is near -20 dBFS, and saturates (does not
  wrap or diverge) on rare full-scale peaks, is spec-compliant.
- A block that requires full-scale *sustained* input to exercise its
  saturation paths is **out of spec** for cocotb stimulus generation --
  such a test is checking a condition normal operation won't produce, and
  a golden-model bug that only appears there (as several did) is real but
  should be understood as "found via out-of-spec stimulus," not "the
  receiver's normal operating range is broken."

## What this resolves

Per the pass-2 review, this contract determines whether the equalizer's
phase-tracking clamp bug and the frequency-correction NCO saturation bug
are "always reachable" or "reachable only out of spec": both are within
normal PAPR peaks (10-13 dB above a -20 dBFS average still leaves several
dB of margin below full scale under most conditions, but PAPR tails do
reach full scale), so both were correctly treated as required fixes, not
edge-case-only bugs -- this doc formalizes that judgment for future
blocks instead of re-litigating it per phase.

## cocotb stimulus guidance

- Steady-state functional tests: generate IQ stimulus centered on -20 dBFS
  average power (matching `AgcController`'s default target), with
  synthetic PAPR peaks reaching but not exceeding full scale.
- Saturation-path tests: explicitly labeled as such, using sustained
  full-scale or overrange input, so a failure there is diagnosed as "the
  saturation logic itself is wrong," not conflated with a steady-state
  regression.
- The three deferred SNR equivalence tests and the Phase 9.0b ≥40 dB
  SNR-vs-float checkpoints should all be generated at the -20 dBFS
  operating point per this contract, not at full scale.

## Register map note

The `-20.0` dBFS target is recorded here as a documented constant, not as
an AXI4-Lite register -- there is nothing for RTL to do with it at
runtime (it governs analog/UHD gain upstream of the RTL boundary, and
RTL never adjusts it). `config/hdl_register_map.json` should reference
this file rather than duplicate the number.
