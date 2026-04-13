# HDL Stubs — AXI4-Stream Interface Templates

This directory contains RTL port-map templates for FPGA implementation of the
gr-atsc3 signal processing chain. These stubs define the AXI4-Stream interfaces
that mirror the C++ `lib/` classes.

## Design Constraints

Per `CLAUDE.md`:

1. **No vendor DSP primitives** — No `DSP48`, no Xilinx/Altera IP. Generic RTL only.
2. **AXI4-Stream contracts** — Every block must document its port contracts.
3. **Fixed-point equivalence** — RTL must match `lib/` fixed-point build behavior.

## AXI4-Stream Signal Convention

All streaming interfaces follow the AMBA AXI4-Stream protocol:

| Signal   | Width    | Direction | Description                     |
|----------|----------|-----------|----------------------------------|
| `TDATA`  | variable | Master→Slave | Payload data                 |
| `TVALID` | 1        | Master→Slave | Data valid                   |
| `TREADY` | 1        | Slave→Master | Backpressure (ready to accept) |
| `TLAST`  | 1        | Master→Slave | End of packet/frame marker   |
| `TUSER`  | variable | Master→Slave | Sideband metadata (optional) |

## Data Types

| C++ Type              | RTL Type        | Notes                          |
|-----------------------|-----------------|--------------------------------|
| `std::complex<float>` | `{re[31:0], im[31:0]}` | IEEE 754 single precision |
| `std::complex<int16_t>` | `{re[15:0], im[15:0]}` | Q1.15 fixed-point        |
| `int8_t` (LLR)        | `[7:0]`         | Signed log-likelihood ratio    |
| `uint8_t`             | `[7:0]`         | Decoded bits                   |

## Block Interface Summary

### FFT Engine (`lib/ofdm/fft_engine.h`)
```
// AXI4-S: TDATA=cf32 TVALID TREADY TLAST(symbol)
Input:  Time-domain OFDM samples (CP removed)
Output: Frequency-domain subcarriers
TLAST:  Asserted on last sample of each OFDM symbol
```

### LDPC Decoder (`lib/fec/ldpc_decoder.h`)
```
// AXI4-S: TDATA=int8(LLR) TVALID TREADY TLAST(codeword)
Input:  Soft LLR values from demapper
Output: Decoded bits
TLAST:  Asserted on last LLR of each codeword
```

### Channel Estimator (`lib/channel/channel_estimator.h`)
```
// AXI4-S: TDATA=cf32 TVALID TREADY TLAST(symbol)
Input:  Pilot subcarriers (scattered + continual)
Output: Channel frequency response estimates
TLAST:  Asserted on last estimate of each symbol
```

## File Organization

```
stubs/
├── README.md                    # This file
├── axi4s_interface.vh           # Common interface definitions
├── fft_engine_stub.v            # FFT wrapper (to be populated)
├── ldpc_decoder_stub.v          # LDPC decoder wrapper (to be populated)
└── channel_estimator_stub.v     # Channel estimator wrapper (to be populated)
```

## Implementation Notes

1. **Pre-allocated buffers** — No dynamic allocation after `init()`. All buffer
   sizes must be known at synthesis time.

2. **Clock domain** — All blocks operate in a single clock domain. Sample rate
   determines clock frequency.

3. **Reset** — Active-high synchronous reset. All state machines must return
   to idle on reset.

4. **Verilator compatibility** — Use C++17 compatible constructs. Avoid
   SystemVerilog features not supported by Verilator 5.x.

## Status

This directory is a **post-MVP** placeholder. RTL stubs will be populated as
each `lib/` block stabilizes and passes equivalence testing.
