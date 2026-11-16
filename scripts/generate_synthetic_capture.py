#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Alfred Wanga
#
"""Generate the synthetic IQ fixtures in test/captures/: A/321 bootstraps
(version 0.0) in a noise-like fill, at the 6.144 MS/s bootstrap rate.

The bootstrap transmitter here is written from A/321 Sections 5.2-5.4 with
numpy, separately from the C++ library and its test-side model:
  - PN: Figure 5.2 LFSR, g(x) = x^16 + x^15 + x^14 + x + 1, output r0,
    seed 0x019D (Table 6.1, minor version 0);
  - ZC: root 137, z(k) = exp(-j pi q k (k+1) / 1499);
  - mapping: s_n(k) = z(k + 749) c((n+1) 749 + k) below DC and
    z(k + 749) c((n+1) 749 - k) above, DC null; last symbol negated;
  - A~_n = IFFT scaled by 1/sqrt(1498) (unit power per sample);
  - Gray-coded relative cyclic shifts (5.3.2), A_n(t) = A~_n(t + M_n);
  - CAB for symbol 0 and BCA after it, from the time-domain formulas of
    5.4.1 and 5.4.2.

Every bootstrap carries the same three signaling fields (recorded in the
.sigmf-meta annotations). The fill between bootstraps is complex Gaussian
at the same power, standing in for the post-bootstrap waveform; a noise
floor 20 dB below it covers everything, and the whole buffer is rotated by
one continuous frequency offset. This exercises detection and bootstrap
decoding end to end; it is not an off-air capture.
"""
import json

import numpy as np

RNG_SEED = 0xA753
FS = 6.144e6
NFFT = 2048
NZC = 1499
NH = (NZC - 1) // 2
SYMBOL = 3072
POWER = 10 ** (-4.85 / 10.0)  # fill and bootstrap power (~ -4.85 dBFS)
SNR_DB = 20.0
CFO_HZ = 973.0293096708543
PERIOD = 1_000_000  # ~163 ms between bootstraps
FIRST = 4096

# ea_wake_up_1=0, min_time_to_next=2 (150-200 ms, Table 6.3 -- matches
# PERIOD), system_bandwidth=00 (6 MHz); ea_wake_up_2=0, bsr_coefficient=2
# (A/322 Table 9.1, 6 MHz); preamble_structure=37 (16K FFT, GI 192,
# Dx 32, L1-Basic Mode 3).
FIELDS = [(0 << 7) | (2 << 2) | 0, 2, 37]


def pn(seed, count):
    r = [(seed >> i) & 1 for i in range(16)]
    out = []
    for _ in range(count):
        out.append(r[0])
        fb = r[0] ^ r[1] ^ r[14] ^ r[15]
        r = r[1:] + [fb]
    return np.array(out)


def gray_shift(bits):
    nb = len(bits)
    m = 0
    for i in range(11):
        if i > 10 - nb:
            v = sum(bits[: 11 - i]) % 2
        elif i == 10 - nb:
            v = 1
        else:
            v = 0
        m |= v << i
    return m


def bootstrap(fields, q=137, seed=0x019D):
    bits = [[(f >> (7 - i)) & 1 for i in range(8)] for f in fields]
    ns = len(bits) + 1
    c = 1 - 2 * pn(seed, (ns + 1) * NH)
    k = np.arange(NZC)
    z = np.exp(-1j * np.pi * q * k * (k + 1) / NZC)
    out = []
    m_abs = 0
    for n in range(ns):
        s = np.zeros(NFFT, complex)
        for kk in range(-NH, 0):
            s[kk % NFFT] = z[kk + NH] * c[(n + 1) * NH + kk]
        for kk in range(1, NH + 1):
            s[kk] = z[kk + NH] * c[(n + 1) * NH - kk]
        if n == ns - 1:
            s = -s
        a_tilde = np.fft.ifft(s) * NFFT / np.sqrt(NZC - 1)
        if n > 0:
            m_abs = (m_abs + gray_shift(bits[n - 1])) % NFFT
        a = np.roll(a_tilde, -m_abs)  # a[t] = a_tilde[(t + M) mod N]
        t = np.arange(SYMBOL)
        sym = np.empty(SYMBOL, complex)
        if n == 0:  # CAB
            sym[:520] = a[(t[:520] + 1528) % NFFT]
            sym[520:2568] = a[t[520:2568] - 520]
            tb = t[2568:]
            sym[2568:] = a[tb - 1024] * np.exp(2j * np.pi * tb / NFFT)
        else:  # BCA
            tb = t[:504]
            sym[:504] = a[tb + 1528] * np.exp(-2j * np.pi * (tb - 520) / NFFT)
            sym[504:1024] = a[t[504:1024] + 1024]
            sym[1024:] = a[t[1024:] - 1024]
        out.append(sym)
    return np.concatenate(out)


def gen_capture(path, duration_sec):
    n_total = int(round(FS * duration_sec))
    rng = np.random.default_rng(RNG_SEED)
    x = (rng.standard_normal(n_total) + 1j * rng.standard_normal(n_total)) * np.sqrt(POWER / 2)
    burst = bootstrap(FIELDS) * np.sqrt(POWER)
    starts = list(range(FIRST, n_total - len(burst), PERIOD))
    for a in starts:
        x[a : a + len(burst)] = burst
    noise_power = POWER * 10 ** (-SNR_DB / 10)
    x += (rng.standard_normal(n_total) + 1j * rng.standard_normal(n_total)) * np.sqrt(
        noise_power / 2
    )
    x *= np.exp(2j * np.pi * CFO_HZ * np.arange(n_total) / FS)
    x.astype(np.complex64).tofile(path + ".sigmf-data")

    meta = {
        "global": {
            "core:datatype": "cf32_le",
            "core:sample_rate": FS,
            "core:version": "1.0.0",
            "core:description": (
                "Synthetic test fixture (not an off-air capture): A/321 version 0.0 "
                f"bootstraps every {PERIOD} samples in complex Gaussian fill, "
                f"{SNR_DB:g} dB SNR, constant CFO. See test/captures/README.md."
            ),
            "core:author": "scripts/generate_synthetic_capture.py",
            "atsc3:synthetic": True,
            "atsc3:cfo_hz": CFO_HZ,
        },
        "captures": [{"core:sample_start": 0}],
        "annotations": [
            {
                "core:sample_start": a,
                "core:sample_count": len(burst),
                "core:description": "A/321 bootstrap, version 0.0, symbol 0 starts here",
                "atsc3:bootstrap_fields": FIELDS,
                "atsc3:cfo_hz": CFO_HZ,
            }
            for a in starts
        ],
    }
    with open(path + ".sigmf-meta", "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")
    return n_total, starts


def main():
    # The "_10sec"/"_20sec" names are provenance labels only; durations are
    # kept just past the deepest scan window any capture-replay test reads
    # (2,000,000 samples), within the repository's git-lfs budget.
    for name, duration in (
        ("test/captures/ch35_10sec_20260503_222404", 0.5),
        ("test/captures/ch35_20sec_20260724_050312", 1.0),
    ):
        n, starts = gen_capture(name, duration)
        print(f"{name}: {n} samples, {len(starts)} bootstraps at {starts}")


if __name__ == "__main__":
    main()
