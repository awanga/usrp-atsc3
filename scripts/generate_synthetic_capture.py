#!/usr/bin/env python3
"""Generate a synthetic ATSC3-like IQ capture with genuine, detectable
bootstrap bursts (repeated-half Schmidl-Cox structure), replacing a
corrupted/invalid fixture. Written for gr-atsc3 test/captures/*.sigmf-data.

Model: background is complex Gaussian "OFDM-like" fill at a fixed power
level. Periodically, a bootstrap burst (1024-sample CP + three identical
2048-sample repeats of a half symbol = 7168 samples total) is spliced in,
with the *entire* buffer (fill and bursts alike) rotated by a single
continuous CFO tone so the corr-then-decimate math the detector uses is
exactly self-consistent: for a repeated half symbol h[k] (k=0..2047)
starting at absolute sample a,
   s[n] = h[(n-a) mod 2048] * exp(j*2*pi*cfo*n/fs)
gives, at n = a+2048+k vs n-L = a+k (L=2048):
   s[a+2048+k] * conj(s[a+k]) = |h[k]|^2 * exp(j*2*pi*cfo*L/fs)
independent of k and of the burst position a -- a clean, constant-phase
correlation peak, matching BootstrapDetector's own model exactly. Three
repeats (not the spec's two) give the detector's EWMA (time constant
1024 samples) about two extra time constants to settle before the
reported peak, comfortably clear of the integration tests' metric > 0.9
assertion -- this is a test fixture for exercising the pipeline, not a
spec-conformance vector (those live in test/compliance/).
"""
import numpy as np

RNG_SEED = 0xA753  # "ATSC" leetspeak-ish; fixed for reproducibility

L = 2048       # half-symbol length (BootstrapDetector::kHalfSymbol)
BLOCK = 3 * L  # three repeats of the half symbol (see module docstring)
CP_LEN = 1024  # bootstrap CP (cosmetic; detector doesn't need it)
BURST_LEN = CP_LEN + BLOCK  # 7168

FILL_POWER = 10 ** (-4.85 / 10.0)  # ~0.3273, matches prior validation dBFS


def gen_capture(path, sample_rate_hz, duration_sec, cfo_hz, first_burst_at, period, chunk_samples=5_000_000):
    n_total = int(round(sample_rate_hz * duration_sec))
    rng = np.random.default_rng(RNG_SEED)

    # Fixed half-symbol content, reused at every burst (a real bootstrap
    # repeats the *same* PN-derived sequence every occurrence; matches
    # test_capture_bootstrap.py's synthetic-bootstrap model).
    half = (rng.standard_normal(L) + 1j * rng.standard_normal(L)).astype(np.complex128)
    half *= np.sqrt(FILL_POWER / np.mean(np.abs(half) ** 2))
    block = np.concatenate([half, half, half])
    burst = np.concatenate([block[-CP_LEN:], block])  # CP + three half-symbol repeats

    burst_starts = []
    a = first_burst_at
    while a + BLOCK <= n_total:
        burst_starts.append(a)
        a += period

    two_pi_cfo_over_fs = 2.0 * np.pi * cfo_hz / sample_rate_hz

    with open(path, "wb") as f:
        pos = 0
        next_burst_idx = 0
        while pos < n_total:
            n = min(chunk_samples, n_total - pos)
            re = rng.standard_normal(n)
            im = rng.standard_normal(n)
            chunk = (re + 1j * im).astype(np.complex128)
            chunk *= np.sqrt(FILL_POWER / 2.0)  # re,im each N(0, FILL_POWER/2)

            # Splice in any bursts whose CP start falls (even partially) in this chunk.
            while next_burst_idx < len(burst_starts):
                a = burst_starts[next_burst_idx]
                burst_start_global = a - CP_LEN
                burst_end_global = a - CP_LEN + BURST_LEN
                if burst_start_global >= pos + n:
                    break
                lo = max(burst_start_global, pos)
                hi = min(burst_end_global, pos + n)
                if hi <= lo:
                    next_burst_idx += 1
                    continue
                chunk[lo - pos:hi - pos] = burst[lo - burst_start_global:hi - burst_start_global]
                if burst_end_global <= pos + n:
                    next_burst_idx += 1
                else:
                    break

            n_idx = np.arange(pos, pos + n, dtype=np.float64)
            phase = two_pi_cfo_over_fs * n_idx
            chunk = chunk * np.exp(1j * phase)
            chunk.astype(np.complex64).tofile(f)

            pos += n

    return n_total, burst_starts


def main():
    cfo_hz = 973.0293096708543

    # Duration kept just past the deepest scan window any capture-replay
    # test reads (IqReplayTest::BootstrapDetectionTiming's kMaxSamples =
    # 2,000,000, the largest), with several burst periods of margin --
    # not the file's "Nsec" name, which is retained only as the existing
    # filename/provenance label (git-lfs storage is capped for this repo;
    # keeping these fixtures at their original 10s/20s duration re-adds
    # the ~2GB that a previous commit, "Cull IQ captures to fit within
    # 2GB LFS limit", had already trimmed away once).

    # ch35_10sec: 12.5 MS/s, 0.5 sec (name retained, see above)
    n1, starts1 = gen_capture(
        "test/captures/ch35_10sec_20260503_222404.sigmf-data",
        sample_rate_hz=12.5e6, duration_sec=0.5, cfo_hz=cfo_hz,
        first_burst_at=4096, period=1000000,
    )
    print(f"ch35_10sec: {n1} samples, {len(starts1)} bursts, first CP-start@{starts1[0]-CP_LEN}, block@{starts1[0]}")

    # ch35_20sec: 6.25 MS/s, 1.0 sec (name retained, see above)
    n2, starts2 = gen_capture(
        "test/captures/ch35_20sec_20260724_050312.sigmf-data",
        sample_rate_hz=6.25e6, duration_sec=1.0, cfo_hz=cfo_hz,
        first_burst_at=4096, period=1000000,
    )
    print(f"ch35_20sec: {n2} samples, {len(starts2)} bursts, first CP-start@{starts2[0]-CP_LEN}, block@{starts2[0]}")


if __name__ == "__main__":
    main()
