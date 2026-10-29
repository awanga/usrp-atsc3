# Synthesis size baseline (Yosys 0.52)

Generic-gate synthesis from `hdl/synth/synth.sh` (no vendor library),
measured 2026-10-29 on Yosys 0.52 (Ubuntu 26.04), all thirteen tops
passing. Cells are the `Number of cells` of the gate-level netlist;
memories are the inferred `$mem_v2` arrays, counted before technology
mapping (they are not in the cell count). Re-measure after a toolchain or
RTL change and update this table; a large jump is a regression to explain.

| Top | Cells | Memories | Memory bits |
|---|---:|---:|---:|
| axi4s_skid_buffer | 214 | 0 | 0 |
| udiv_seq | 1,183 | 0 | 0 |
| isqrt_seq | 1,757 | 0 | 0 |
| cp_removal | 608 | 0 | 0 |
| pilot_extractor | 1,525 | 3 | 46,848 |
| cordic | 4,245 | 1 | 196 |
| polyphase_fir | 5,361 | 1 | 8,192 |
| freq_correction | 11,084 | 1 | 196 |
| fft_engine | 16,850 | 3 | 2,621,440 |
| timing_recovery | 17,725 | 2 | 12,288 |
| frame_correlator | 31,365 | 2 | 708 |
| bootstrap_detector | 42,446 | 3 | 98,500 |
| frame_sync | 55,036 | 3 | 2,097,860 |

Formal wall-clock: every job except `pilot_extractor` closes inside the
default 900 s limit of `run_formal.sh`; `pilot_extractor` (yices) needs
32-58 minutes (58 on 2026-10-29 with other jobs running), so run it with `FORMAL_TIMEOUT=4000`.
