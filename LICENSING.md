# Licensing

gr-atsc3 is free software under weak-copyleft licenses: anyone may use,
embed and ship it, commercially or not, but modified versions of these
files must be distributed with their source under the same license.

| Path | License | Text |
|------|---------|------|
| Everything not listed below (`lib/`, `hal/`, `blocks/`, `apps/`, `av/`, `python/`, `ml/`, `config/`, `scripts/`, `test/`, build files) | MPL-2.0 | [`LICENSE`](LICENSE), [`LICENSES/MPL-2.0.txt`](LICENSES/MPL-2.0.txt) |
| HDL design sources (`hdl/rtl/`, Verilog in `hdl/stubs/`) | CERN-OHL-W-2.0 | [`LICENSES/CERN-OHL-W-2.0.txt`](LICENSES/CERN-OHL-W-2.0.txt) |
| HDL testbenches, formal harnesses and scripts (`hdl/sim/`, `hdl/formal/`, `hdl/synth/`, `hdl/mutants/`) | MPL-2.0 | as above |
| Documentation (`*.md`) | CC-BY-4.0 | [`LICENSES/CC-BY-4.0.txt`](LICENSES/CC-BY-4.0.txt) |

Every source file states its license in an `SPDX-License-Identifier`
header; files that cannot carry one are covered by [`REUSE.toml`](REUSE.toml).

## What the licenses require

- **MPL-2.0** is file-level copyleft. Distributing a modified version of
  an MPL-2.0 file means making that file's source available under
  MPL-2.0. New files of your own, and the larger program they are
  combined with, can be under any license, including proprietary ones.
- **CERN-OHL-W-2.0** applies the same idea to hardware. It covers the RTL
  and anything made from it (bitstreams, netlists, devices). Modified
  RTL must be released, while the RTL can still be combined with closed
  designs (see its "Available Component" terms).
- Neither license obliges anyone to send changes back to this project;
  contributions are welcome through pull requests (see below).

## Combined works and GPL dependencies

MPL-2.0 is compatible with the GPL (MPL-2.0 Section 3.3). Some binaries
link GPL code, and those binaries are then distributed under the GPL as a
whole, while the source files here keep their own license:

| Component | Links | Resulting binary |
|-----------|-------|------------------|
| `blocks/`, `python/` (GNU Radio module) | GNU Radio (GPL-3.0) | GPL-3.0 |
| `hal/` UHD source | UHD (GPL-3.0) | GPL-3.0 |
| `lib/` float build with FFTW3 installed (the default prefers it for speed) | FFTW3 (GPL-2.0+) | GPL |
| `lib/` with `-DATSC3_USE_FFTW=OFF`, or the fixed-point build | Boost (BSL-1.0) only | MPL-2.0 |
| `av/` | FFmpeg, GStreamer (LGPL-2.1+, linked dynamically) | MPL-2.0 (keep FFmpeg built without `--enable-gpl`) |

`lib/` contains no copyleft code of its own: the NUC constellation tables
are generated from the ATSC A/322 standard by `scripts/gen_nuc_tables.py`.

To ship `lib/` without any copyleft dependency, configure the float
build with `-DATSC3_USE_FFTW=OFF` (or buy MIT's commercial FFTW license).

## Patents

ATSC 3.0, HEVC and AC-4 are covered by patent pools. These licenses grant
only the contributors' own patent rights; commercial receivers need
licenses from the relevant pools regardless of this project's licensing.

## Contributing

Contributions are accepted under the license of the file they change (or
MPL-2.0 for new files, CERN-OHL-W-2.0 for new RTL), certified with a
Developer Certificate of Origin sign-off (`git commit -s`; text at
<https://developercertificate.org/>).
