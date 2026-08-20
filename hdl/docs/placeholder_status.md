# Non-Spec Placeholders in the Golden Model

> Phase 9.0 deliverable. RTL is required to be bit-exact against `lib/`'s
> fixed-point golden model (Decision 7). Where the golden model itself is
> not ATSC A/322-compliant, RTL faithfully ports the golden model's actual
> (non-spec) behavior, documented here rather than silently treated as
> correct. Every entry is swappable later via a data-only or algorithm
> change once/if the underlying C++ is fixed, without changing this
> milestone's RTL-vs-golden-model equivalence goal.

| # | Component | Golden model reference | What it actually does | Real ATSC A/322 behavior |
|---|---|---|---|---|
| 1 | LDPC H-matrices | `lib/fec/ldpc_decoder.cc` | Non-spec placeholder matrices (documented in Phase 9.11) | Standardized per-rate QC-LDPC matrices, A/322 Annex |
| 2 | L1 signaling FEC | `lib/framing/l1_decoder.cc` (`fec_decode()`) | Bare LLR hard-decision sign-slicer; `short_ldpc`/`code_rate` params explicitly discarded. Comment: *"Post-MVP: Integrate full LDPC+BCH decoding for L1 signaling (currently uses hard decision)."* | L1-Basic/L1-Detail protected by their own LDPC+BCH coding |
| 3 | Frame sync preamble reference | `lib/sync/frame_sync.cc` (`:30-50`) | Throwaway pseudo-random pattern, `(k*7)%13` phase sequence | Real ATSC 3.0 bootstrap/preamble symbol structure |
| 4 | 16K/32K continual pilot positions | `lib/ofdm/pilot_extractor.cc` (`:222-244`) | Computed at runtime by scaling + interpolating the 8K list | Should be tabulated per A/322 pilot pattern tables |
| 5 | Pilot PRBS reference | `lib/ofdm/pilot_extractor.cc` | No PRBS-based pilot value reference; positions only | A/322 defines pilot boosting values via a PRBS sequence |
| 6 | Cell deinterleaver permutation | `lib/ofdm/cell_deinterleaver.cc` (`:174-175`) | Bit-reversal permutation | Real ATSC A/322 cell interleaving (not bit-reversal) |
| 7 | Frequency deinterleaver permutation | `lib/ofdm/freq_deinterleaver.cc` (`:161-162`) | Bit-reversal permutation | Real ATSC A/322 frequency interleaving. `docs/compliance.md` and `TASKS.md` §8.1 disagree on whether this is "LFSR" or bit-reversal -- the code is bit-reversal; those docs are stale, not this one |
| 8 | ALP ROHC decompression | `lib/framing/alp_demux.cc` (`:253-256`) | `COMPRESSED` (ROHC) ALP packets routed to the plain IPv4 handler with no actual decompression | RFC 3095-family ROHC decompression |

## Also non-spec, but load-bearing rather than swappable-later

These aren't in the "swap the data/algorithm later" category above --
they're approximations in otherwise-real algorithms, kept because the
golden model's own comments identify them as intentional:

- **Bootstrap detector correlation** (`lib/sync/bootstrap_detector.cc`):
  the P-sum (correlation) term is an EWMA approximation
  (`p_sum_ = (1-α)·p_sum_ + α·corr_term·kHalfSymbol`), not a true
  2048-sample sliding-window sum. The code's own comment flags this as an
  approximation of the true Schmidl-Cox metric. RTL mirrors the EWMA
  (Phase 9.1) since that's what bit-exactness is measured against, not
  because it's spec-accurate.
- **NUC constellation compliance**: real code-rate-indexed NUC tables do
  exist (`lib/ofdm/nuc_tables.h`, credited to gr-atsc3/Ron Economos,
  GPL-3.0 -- confirm license compatibility per `CLAUDE.md`'s Licensing
  section before this table is ported into RTL). `TASKS.md` Phase 7.3
  tracks 6/11 NUC compliance test failures as an open correctness bug in
  the golden model itself, not a placeholder; RTL inherits whatever the
  golden model does until that's fixed upstream.

## Not carried into RTL scope at all

- **`check_reassembly_timeouts()`** (`lib/framing/alp_demux.cc:440-452`)
  is unimplemented in the C++ (a `static` shared-across-instances no-op).
  RTL does not build a timeout mechanism to match it -- there is nothing
  to be bit-exact against. If ALP reassembly timeout handling is wanted
  in RTL, it is new design work against no reference (same caveat class
  as the Phase 9.13b config sequencer).

## Maintenance note

If any row in the first table is fixed in `lib/` (a real LDPC matrix
lands, real interleaving replaces bit-reversal, etc.), the corresponding
RTL block's golden-model equivalence tests will start failing against
the *old* RTL until the RTL is regenerated/re-verified against the new
reference -- that's expected, not a regression. Update this table in the
same change that updates the golden model.
