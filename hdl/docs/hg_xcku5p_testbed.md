# HamGeek hg_xcku5p test bed: firmware plan and trade study

> Status: plan, nothing built. Written 2026-10-18 from the vendor's sales
> page plus the user's feature list. No schematic, constraint file or
> example project has been read yet, so every pin, clock and connector
> statement below marked **verify** must be confirmed from the vendor's
> download package before RTL depends on it.

## Goal

Run the OpenATSC3 receive chain (hdl/rtl, ADC samples in, ALP out) on a
real FPGA with a USRP N2x0 + TVRX as the RF front end, so the RTL is tested
against live ATSC 3.0 signals, not only golden vectors:

1. Control the N2x0 over Ethernet: tune channel, gain, sample rate; select
   service / audio stream.
2. Receive its sample stream with minimal drops and present it as AXI4-S
   `ci16` to the receiver blocks.
3. Show signal quality / strength on the board's LEDs.
4. Produce audio and video through the HDMI output.

## Board facts and gaps

Source: the vendor product page (SKU 101069, "XCKU5P-FFVB676-2-i").

| Item | Stated | Not stated, verify from schematic |
|---|---|---|
| FPGA | XCKU5P-FFVB676-2-i, 475K logic cells, 217K LUT, 1824 DSP, 16.9 Mb BRAM, 18 Mb URAM | |
| DDR4 | 2 x MT40A256M16GE-075E, 2666 MT/s | Whether the two chips form one 32-bit bus or two 16-bit buses; capacity (2 x 4 Gb = 1 GB if 32-bit) |
| Transceivers | "16 pair of 32.75 G GTY" (page also says GTH once) | Which quads go to the 100G cage, FMC, PCIe |
| Ethernet | CAT6 GbE cable included | PHY part, RGMII vs SGMII, which bank |
| 100G | title only | cage type (QSFP28?), quad |
| HDMI | "HDMI-compatible" in the title | TX or RX, direct-IO TMDS or via a retimer/transmitter chip, connector |
| FMC | "FMC HPC" per the user's list | Pin count, VADJ, which banks |
| User I/O | 8 LEDs, 4 DIP switches, 4 keys, buzzer (user's list) | Pin numbers, voltage bank |
| Clocks | PLL max 775 MHz | Every oscillator frequency |
| Other | SD, 2 x BL24C256, BL24C02, MS5611, DS18820 sensor, flash, USB-C UART/JTAG (user's list) | |
| Power | 12 V 2 A adapter, USB-C cable included | |
| Availability | page shows sold out; $289 base, up to $449 with an AD9361 module | |

Consequences: the HDMI path has the largest unknown. If the connector is
driven straight from FPGA I/O, an open TMDS serializer (OSERDES, plain
differential outputs, 720p60 at 74.25 MHz pixel clock, 742.5 Mb/s per lane)
is a generic-RTL job. If it goes through a transmitter chip, that chip
needs an I2C configuration sequence from the control processor. Either way
the video path must be bit-exact only up to the AXI4-S pixel stream; the
TMDS/I2C part is board-specific.

## Where vendor primitives are allowed

The project rule "no vendor primitives" applies to `hdl/rtl/`, the
portable receiver. The board top (`hdl/boards/hg_xcku5p/`, new) is
inherently vendor specific: clocking (MMCM), DDR4 controller, GT / MAC
cores, OSERDES for HDMI, IO standards. Keep every such instance in that
directory, behind plain AXI4-S / AXI4-Lite ports, so `hdl/rtl/` stays
simulatable in Verilator and Icarus. The Xilinx toolchain (Vivado) is not
installed here and not open source; an open flow for UltraScale+ (e.g.
Project X-Ray based) does not cover the GTY transceivers or DDR4 PHY.
Decision needed from the user: Vivado is acceptable for the board build
only.

## Trade: control plane as a state machine or a CPU

The N2x0 control protocol (UHD 3.15, from the project's HAL notes and UHD
documentation, **verify against the UHD source**): device discovery by UDP
broadcast, then register peek/poke and stream commands over UDP to the
device's control port (49152), sample data returning in VITA-49-like
packets on a data port (49153), at gigabit Ethernet. The TVRX tuner is not
programmed by the N2x0 itself: UHD's host-side daughterboard code computes
the tuner and gain settings and writes them through the device's I2C/SPI
and GPIO registers. So "tune a channel" from the FPGA means reimplementing
that host-side logic.

| | Hardwired state machine | Soft CPU (RISC-V core, open, e.g. VexRiscv or Ibex, with firmware) |
|---|---|---|
| Cost in logic | Few hundred LUTs per command | ~2-5k LUT + 32-128 KB BRAM, negligible on this device |
| Tuner programming | Fixed command tables per channel; every new frequency / gain curve is RTL | Port the TVRX tuner math to C; frequency in = registers out |
| Discovery, ARP, retries, timeouts, sequence numbers | Awkward; each error path is more states | Ordinary code |
| Service / audio selection, UI (keys, switches) | Fixed menus | Scripted; reads L1 / ALP tables from AXI4-Lite |
| Verification | Fits the project's formal + golden-vector bar | Firmware is C: tests are host-side unit tests; the CPU itself is third-party |
| Licensing | Own HDL (CERN-OHL-W) | Core licence must be compatible (VexRiscv MIT, Ibex Apache-2.0) |
| Determinism | Cycle exact | Not needed on the control path |
| Risk | Protocol drift = RTL change | Core and toolchain dependency |

**Recommendation: CPU for control, hardware for data.** Control traffic is
low rate and branchy (discovery, tuning, error handling, UI); that is what
firmware is good at, and the protocol knowledge that must be ported (TVRX
tuning) is arithmetic that is easier to check in C against UHD than as
RTL. The data path stays in hardware: Ethernet MAC -> UDP/VITA-49 depacketizer
-> sample FIFO -> AXI4-S `ci16`, with no CPU involvement per packet. A
state machine remains appropriate for the narrow, fixed jobs: start/stop
streaming, packet header parse, sequence-gap detection, LED driver.

Alternative worth costing: host-assisted bring-up, where the existing
host stack (UHD on a PC) programs the N2x0 and the FPGA only receives the
stream. That is the fastest path to first data and de-risks the RTL, but
does not meet the "firmware controls the USRP" goal; use it as step 1.

## Data path and drop budget

- Rate: 6.25 MS/s x 32 bit (ci16) = 200 Mb/s of payload, about 20% of
  gigabit Ethernet. The N2x0 sends 16-bit IQ at up to 25 MS/s (800 Mb/s);
  the project's 6.25 MS/s keeps headroom.
- Drops: UDP has no retransmission. Size the receive FIFO for the worst
  stall between the MAC and the receiver chain. The receiver is
  currently not real time (freq_correction 18 vs 16 cycles/sample, FFT about
  192k cycles per 8K symbol vs 147k available; see TASKS.md), so the board
  cannot run live until those close or a symbol-sized DDR4 buffer decouples
  them. DDR4 (about 1 GB) holds minutes of samples and is also the natural
  place for the time deinterleaver later.
- Detect, don't hide: count VITA sequence gaps and FIFO overflows in
  AXI4-Lite registers; light an LED on any drop.
- Clock: the receiver's nominal clock is 100 MHz; the 6.25 MS/s sample
  clock and the Ethernet reference are asynchronous, so the FIFO needs a
  proper clock-domain crossing and its own formal harness.
- GbE vs the 100G port: the N2x0 only has gigabit Ethernet. The 100G cage is
  not needed for this test bed; leave it unused.

## Status LEDs (8 on the board)

Proposed, from signals the RTL already exposes (frame_sync monitors,
packet counters); final pinout after the schematic is read:

| LED | Meaning | Source |
|---|---|---|
| 0 | Ethernet link up | MAC / PHY |
| 1 | Samples arriving | depacketizer |
| 2 | Drop seen (sticky, clear with a key) | sequence gap / FIFO overflow |
| 3 | Bootstrap detected | bootstrap_detector |
| 4 | frame_sync ACQUIRING or LOCKED | `mon_state` |
| 5 | frame_sync LOCKED | `mon_state` |
| 6-7 | Signal quality bar (4 levels) from `mon_confidence` | frame_sync |

Signal-strength numbers (RSSI in dBm, SNR in dB) need `log10`, which the
RTL deliberately excludes; take RSSI from the N2x0 sensor over the control
link (CPU path) and show it on the bar.

## HDMI output

Audio and video are the end of the chain (HEVC / AC-4 decode today lives in
FFmpeg on a host). A decoder in FPGA fabric is out of scope for the RTL
port. Options, in order of effort:

1. Test pattern / status page only: a small character generator and the
   quality bar rendered to 720p60. Needs just the TMDS output. Proves the
   HDMI path and gives on-screen diagnostics.
2. Decode on a host: stream the ALP / transport stream out over the 100G
   or GbE port to a PC running the existing av/ code; HDMI stays unused.
3. Soft decode on the board CPU: not realistic for HEVC video; possible for
   audio (AC-4 licensing aside).

Recommendation: do (1) first, then (2) for real content.

## Phased plan

1. Read the vendor's schematic / constraints; fill the **verify** column.
   Blocked on obtaining the download package from the vendor (the sales page
   links it but its contents are not visible here).
2. Board top with clocks, LEDs, keys, UART, and a self-test; no RF.
3. GbE MAC + UDP receive of a recorded N2x0 packet capture replayed from
   a PC (no USRP needed); depacketizer with gap counters, formal on the
   FIFO crossing.
4. Soft CPU + firmware: discovery and streaming start against a real
   N2x0; host-assisted tuning first, then the tuner port.
5. Connect `hdl/rtl` receive chain at the throughput the board can sustain;
   LED status; ILA-style capture to DDR4 for debug.
6. HDMI status page, then host-side decode of the ALP stream.

## Open questions for the user

- Is Vivado (proprietary) acceptable for the board build?
- Does the vendor ship schematics and example projects? Please share the
  download package or confirm access.
- Is the AD9361 variant in play? It would be an alternative RF front end to
  the USRP path.
