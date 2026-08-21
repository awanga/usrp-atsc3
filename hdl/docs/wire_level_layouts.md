# Wire-Level Layouts for Fixed-Size Status/Sideband Structs

> Phase 9.0 deliverable (see the HDL port plan's Phase 9.0 bullet on
> `PilotSymbol`, `BootstrapDetection`, `FrameEvent`, `DemapResult`, and
> L1Pre/L1Post field packing). Bit-range constants live in
> `hdl/rtl/include/status_words.vh`; this doc is the rationale.

## Which lib/ structs get a packed wire layout, and which don't

Four of the five structs the plan named are small, fixed-size, output-once-
per-event structs -- real sideband/status words that need a bit-packed RTL
representation: `PilotSymbol`, `BootstrapDetection`, `FrameEvent`, and (see
below) L1Pre/L1Post. **`DemapResult` does not get one.** Its dominant field,
`llr` (`std::vector<int8_t>`), *is* the `int8_t`(LLR) AXI4-S stream already
specified in `hdl/stubs/README.md` and the HDL port plan's Phase 9.9 --
one LLR per beat, `TLAST` per symbol/codeword boundary. Packing `DemapResult`
into its own status word would create a second, redundant representation of
data that's already flowing on the primary stream. Its other three fields
don't need wire representation either:

- `num_symbols` -- the receiver already knows this by counting beats
  between `TLAST` pulses; not signaled per-beat.
- `bits_per_symbol` -- derivable from the `MODULATION` register
  (`config/hdl_register_map.json`, `l1_status` bank); redundant to signal
  again per-beat.
- `is_valid` -- `lib/ofdm/constellation_demapper.cc` only ever sets this
  `true` (line 593); there is no golden-model code path that produces a
  populated `DemapResult` with `is_valid == false`. It's effectively a
  dead field today (`false` only as the struct's default-constructed
  value, never as a real "demap failed" signal). RTL should not build
  wire logic for a failure mode the golden model doesn't actually have --
  if this becomes a real signal later, it's a `TVALID`-adjacent or
  single-bit `TUSER` concern on the existing LLR stream, not a field in a
  separate struct.

L1Pre/L1Post similarly don't need a *new* packed format: every one of their
fields already has an explicit offset/width in the `l1_status` bank of
`config/hdl_register_map.json` (AXI4-Lite registers, written once per
acquisition by the Phase 9.13b sequencer -- these fields don't stream on
`TDATA` the way samples do, so a register file, not a bit-packed word, is
the right wire representation). That JSON is the answer to this part of the
Phase 9.0 bullet; nothing new was needed here.

## Packing convention (applies to all four `.vh` layouts)

- LSB-first, each field individually byte-aligned. No sub-byte packing
  across field boundaries other than explicit 1-bit flags, which still own
  a full reserved byte (keeps field extraction a single slice, no
  cross-byte shifting for anything but the flag bit itself).
- Every `complex<int16_t>` field packs as `{re[15:0], im[15:0]}` -- re in
  the upper half, im in the lower half. This matches the data-type table
  already in `hdl/stubs/README.md`; kept identical rather than inventing a
  second convention.
- `q1_15` fields use the same truncating (not rounding) scale-32768
  convention as `config/hdl_register_map.json` and `hdl/docs/q_format_notes.md`.

## Fields deliberately left out: the log10 line

`BootstrapDetection.snr_db` is **not** in `status_words.vh`.
`lib/sync/bootstrap_detector.cc` computes it with `std::log10`. Phase
9.0b's planned CORDIC core covers rotation mode (cos/sin) and vectoring
mode (atan2/magnitude) -- log10 needs CORDIC's hyperbolic mode, a separate
design the plan explicitly scopes out this milestone (same reasoning as
`config/hdl_register_map.json`'s excluded `lib/metrics/*` registers). This
is the same exclusion class, just discovered in a non-metrics file: the
line isn't "which directory is it in," it's "does producing this value
need log/exp." `FrameEvent.confidence`, by contrast, comes from
`frame_sync.cc`'s `sqrt`-based normalization (per the HDL port plan's
Phase 9.6 section) -- vectoring-mode CORDIC covers `sqrt`, so it's included
as a normal `q1_15` field with no asterisk.

If a future block's status word has a field computed via `log10`/`exp`,
apply this same test before deciding whether it belongs in a `.vh` layout
yet.

## Absolute vs. relative subcarrier indexing (pinned once)

The gap the plan flagged: `PilotSymbol.subcarrier_index` is documented as
an "FFT bin index" (`lib/ofdm/pilot_extractor.h:47`) -- absolute, 0 at DC/
the FFT's first bin. But `Equalizer::equalize()` takes `received`/
`channel_estimate` arrays indexed `0..num_active_carriers-1`, relative to
the active-carrier window (`lib/channel/equalizer.h:114-116`: "FFT output
for active subcarriers"). Two different structs in the same signal chain
use two different index origins for what's conceptually the same axis.

**Rule, pinned once here, applies everywhere on the RTL datapath:**

```
first_active_carrier = (fft_size - num_active_carriers) >> 1
relative_index        = absolute_index - first_active_carrier
```

- `fft_size` and `num_active_carriers` both come from the `l1_status`
  register bank -- `first_active_carrier` is **not** its own register.
  Every block that needs it computes this one-line formula internally
  (a subtract and a shift; matches `lib/ofdm/pilot_extractor.cc`'s
  `compute_first_active()` exactly). Giving it a register would be a
  redundant, independently-writable copy of a value that's supposed to be
  a pure function of two other registers.
- `received[]` / `channel_estimate[]` / equalized-symbol / LLR streams are
  **positional**: beat N on those AXI4-S streams *is* relative subcarrier
  index N. No explicit index field rides along on the wire for them.
- `PilotSymbol` is the one structure that names a subcarrier explicitly,
  and it is always absolute, matching the C++ struct precisely. Any block
  that needs to correlate a `PilotSymbol.subcarrier_index` against a
  position in a relative-indexed array/stream must subtract
  `first_active_carrier` first -- this was previously implicit and
  easy to get backwards; it's explicit now.

## Layout summary

| Struct | `.vh` width define | Total width | Notes |
|---|---|---|---|
| `PilotSymbol` | `PILOT_SYMBOL_WIDTH` | 96 bits (12 B) | index + 2×complex + type + 1 reserved byte |
| `BootstrapDetection` | `BOOTSTRAP_DETECTION_WIDTH` | 104 bits (13 B) | `snr_db` excluded (log10, see above) |
| `FrameEvent` | `FRAME_EVENT_WIDTH` | 136 bits (17 B) | belongs to frame_sync (Phase 9.6), not timing_recovery -- corrected in the HDL port plan from an earlier draft |
| `DemapResult` | *(none)* | -- | already the existing `int8_t`(LLR) AXI4-S stream; see above |
| L1Pre / L1Post | *(none)* | -- | already `config/hdl_register_map.json`'s `l1_status` bank |

Actual AXI4-S `TDATA` bus width for `PilotSymbol`/`BootstrapDetection`/
`FrameEvent` may pad these up to a rounder bus width (e.g. 128/144 bits) at
block-integration time in their respective phases (9.5/9.1/9.6) -- that's
an integration-time decision for those phases, not fixed here. The byte-
aligned minimal widths above are what matters for this deliverable: every
field has an unambiguous offset and width today.

## 48-bit sample counters: a wraparound check worth keeping

`BootstrapDetection.sample_index` and `FrameEvent.sample_index` are 48
bits, not the seemingly-natural 32. At the plan's nominal 6.25 MS/s, a
32-bit counter wraps in `2^32 / 6.25e6 ≈ 687 s ≈ 11.5 minutes` --
clearly wrong for a receiver meant to run continuously. 48 bits gives
`2^48 / 6.25e6 ≈ 4.5×10^7 s ≈ 1.4 years`. Worth restating if either
sample counter's width is ever "optimized" back down without re-deriving
this bound.
