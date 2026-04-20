# CLAUDE.md — gr-atsc3 Agent Context

> Full project spec: `README.md` | Active task plan: `TASKS.md`

## What This Project Is

GNU Radio OOT module implementing an ATSC 3.0 physical-layer receiver
for USRP N2x0 + TVRX daughterboard (UHD 3.15). C++17 core DSP library
wrapped by GR blocks. Architected for FPGA RTL portability.

---

## Absolute Architectural Rules

1. **`lib/` has zero GR/UHD headers.** All DSP lives here. Pure C++17 + FFTW3 + Boost only.
2. **`blocks/` delegates immediately to `lib/`.** No DSP logic in GR blocks.
3. **`hal/IQSource` is the only UHD touch point.** UHD version differences are gated by `UHD_VERSION` macros inside `hal/uhd_source.cc` only.
4. **Every `lib/` class must compile with `-DATSC3_FIXED_POINT=ON`** using `int16_t`/`int32_t` Q-format. Float and fixed builds must have equivalence tests.
5. **No dynamic allocation after `init()`.** Pre-allocate all buffers. RTL has no heap.
6. **No vendor DSP primitives anywhere.** No `DSP48`, no Xilinx/Altera IP. Generic RTL only.
7. **AXI4-Stream port contracts documented in every block header.** Comment format: `// AXI4-S: TDATA=cf32 TVALID TREADY TLAST`

---

## Directory Map

```
hal/         IQSource interface + UHDSource / FileSource / NullSource
lib/         Core DSP: sync/ ofdm/ channel/ fec/ framing/ metrics/
blocks/      GR OOT wrappers (thin; no DSP)
apps/        atsc3_rx.grc, scanner.py, metrics_server.py
av/          FFmpeg decode + GStreamer playback
hdl/         RTL port stubs and AXI4-S interface defs (post-MVP)
ml/          PyTorch models + ONNX Runtime inference (post-MVP)
test/        unit/, integration/, captures/ (IQ files via git-lfs)
docker/      CI reproducible build environment
.github/     ci.yml, nightly.yml, release.yml
```

---

## Build Commands

```bash
# Standard build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)

# Fixed-point build
cmake .. -DATSC3_FIXED_POINT=ON && cmake --build . -j$(nproc) --target atsc3_fxp_tests

# Run unit tests only (no hardware, no IQ files)
ctest -L unit --output-on-failure

# Run all tests (needs git-lfs IQ captures)
ctest --output-on-failure -j$(nproc)

# Format check
find lib blocks hal -name '*.cc' -o -name '*.h' | xargs clang-format --dry-run --Werror

# Static analysis
cppcheck --enable=all --error-exitcode=1 lib/ blocks/ hal/
```

---

## Coding Conventions

- **Language:** C++17; no C++20 features (Verilator compat)
- **Naming:** `snake_case` for all identifiers; class names `PascalCase`
- **Headers:** `#pragma once`; no `using namespace` in headers
- **Fixed-point:** Use `ATSC3_SAMPLE_T` typedef (resolves to `std::complex<float>` or `std::complex<int16_t>`)
- **Tests:** One `.cc` file per `lib/` class in `test/unit/`; use GoogleTest
- **Parameters:** No magic numbers; all ATSC 3.0 mode tables loaded from `config/` JSON at runtime
- **Error handling:** Use `std::expected` (backported via Boost.Outcome) not exceptions in `lib/`; GR blocks may throw

### Q1.15 Fixed-Point Format (Persistent Requirement)

All fixed-point DSP code must use **16-bit Q1.15 format** for RTL compatibility:

- **Container:** `int16_t` (16 bits total)
- **Bit layout:** 1 sign bit (MSB) + 15 fractional data bits
- **Scale factor:** 32768 (2^15)
- **Range:** [-1.0, +1.0) represented as [-32768, +32767]
- **Resolution:** 2^-15 ≈ 3.05e-5
- **Saturation:** Clamp intermediates to `int16_t` range before storage
- **Complex samples:** `std::complex<int16_t>` (real and imag both Q1.15)
- **Helpers:** Use `float_to_q15()` / `q15_to_float()` from `lib/types.h`

**Note:** `float_to_q15(1.0f)` overflows — use 32767 for +1.0 representation.

### Q1.15 Fixed-Point Requirements (RTL Compatibility)

All fixed-point code must conform to hardware Q1.15 format for RTL portability:

- **Data type:** `int16_t` only (1 sign bit MSB + 15 fractional bits)
- **Range:** [-1.0, +1.0) represented as [-32768, +32767]
- **Max positive value:** 32767 (≈ 0.999969), NOT 32768 (overflows to -32768)
- **Intermediate calculations:** May use `int32_t` or `int64_t` to prevent overflow, but must be scaled/saturated back to `int16_t` before storage
- **Saturation:** Always clamp to [-32767, +32767] (symmetric) to avoid asymmetric overflow behavior
- **Conversion helpers:** Use `float_to_q15()` and `q15_to_float()` from `lib/types.h`; never use `float_to_q15(1.0f)` directly (overflows)
- **BPSK values:** Use ±32767 (not ±32768) for ±1.0 representation
- **Complex samples:** `std::complex<int16_t>` with both real and imag in Q1.15

**Rationale:** This matches typical FPGA DSP pipeline widths and enables direct RTL translation without numerical divergence.

---

## Key Interfaces

```cpp
// hal/iq_source.h
class IQSource {
    virtual void   set_frequency(double hz)    = 0;
    virtual void   set_gain(double db)         = 0;
    virtual void   set_sample_rate(double sps) = 0;
    virtual size_t read(std::complex<float>* buf, size_t n) = 0;
    virtual double get_rssi_dbm()              = 0;
};

// lib/ofdm/fft_engine.h — AXI4-S: TDATA=cf32 TVALID TREADY TLAST(symbol)
void process(const ATSC3_SAMPLE_T* in, ATSC3_SAMPLE_T* out, size_t fft_size);

// lib/fec/ldpc_decoder.h — AXI4-S: TDATA=int8(LLR) TVALID TREADY TLAST(codeword)
bool decode(const int8_t* llr_in, uint8_t* bits_out, size_t codeword_len, int max_iter);
```

---

## CI/CD Summary

| Workflow       | Trigger       | Key checks                                      |
|----------------|---------------|-------------------------------------------------|
| `ci.yml`       | Every PR      | Build, unit tests, integration/IQ replay, lint  |
| `nightly.yml`  | 02:00 UTC     | Long IQ tests, GR 3.10 compat, coverage, docs   |
| `release.yml`  | `v*` tag      | Full suite + `.deb` package + GitHub Release    |

IQ test captures tracked via **git-lfs** in `test/captures/`.
PR merge requires `ci.yml` green. No force-push to `main`.

---

## ATSC 3.0 Signal Chain (abbreviated)

Bootstrap detect → Frame sync → CP removal → FFT →
Channel estimate (LS+Wiener) → FDE equalize → LLR demap →
De-interleave → LDPC decode → BCH decode → ALP demux →
FFmpeg HEVC/AC-4 decode → GStreamer playback

L1-Pre/Post preamble configures all OFDM parameters dynamically.
See `README.md §Signal Chain` for full detail with lib/ references.

---

## Post-MVP Hooks (do not remove)

- `lib/channel/channel_estimator.h` has a `setBackend(EstimatorBackend)` enum slot reserved for ML backend.
- `hdl/stubs/` contains port-map templates — populate as each `lib/` block stabilizes.
- `ml/` directory skeleton exists; do not add ML dependencies to `lib/` CMakeLists.

---

## Common Pitfalls

- TVRX RSSI register is read via UHD sensor API (`"rssi"`), not from baseband.
- ATSC 3.0 bootstrap is always 4K OFDM regardless of data FFT size — do not use L1 FFT size for bootstrap correlation.
- LDPC H matrices are stored sparse (row, col lists); do not expand to dense — will OOM for 64800-bit codewords.
- GStreamer pipeline must be built on the main thread; FFmpeg decode workers can be threaded.
- UHD 3.15 `multi_usrp::make()` API differs from UHD 4.x `rfnoc_graph` API — all differences go in `hal/uhd_source.cc` only.
