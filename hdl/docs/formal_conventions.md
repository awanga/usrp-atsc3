# Formal Verification Conventions

> Established and verified against
> `hdl/rtl/common/axi4s_skid_buffer.v` / `hdl/formal/axi4s_skid_buffer.sby`
> before any algorithmic block's formal harness was written. Follow this
> pattern for every subsequent block's formal work.

## What each harness must contain

- **A main property that checks data integrity against a ghost model**,
  not just flag and range bounds: harness-side registers that record
  what the DUT accepted (operands at a start edge, a beat at a
  solver-chosen index K via `(* anyconst *)`, a count of accepted beats)
  and assertions that the DUT delivers exactly that. See
  `axi4s_skid_buffer_formal.v` (beat K leaves once, in order,
  unmodified) or `udiv_seq_formal.v` (quotient == a / b, exact latency).
  Where the data itself has no tractable formal oracle (FFT, CORDIC,
  correlator values), the ghost covers the data path that does (load
  addressing, input buffering, sample counting) and the header says what
  is left to cocotb and why.
- **Assumptions that each cite the caller contract they encode** (e.g.
  "configuration is latched at reset; reconfiguration is by reset", or
  the AXI4-S producer hold rule). An assumption with no contract behind
  it is a hole in the proof.
- **A cover for the key event**, or, when that event is deeper than any
  practical BMC depth (a completed 8K symbol, the 128-cycle clear sweep),
  a header note saying so and naming the committed mutants that show the
  properties are load-bearing instead.
- **A header** stating the depth versus the deepest event, what was
  shrunk, and what is cut.

All blocks so far are single-clock. A single shared clock proves protocol
and data integrity, not metastability; a clock-domain crossing needs its
own harness and a note saying exactly that.

## SVA-lite, not full SVA

`rtl/` modules are IEEE 1364-2001 Verilog and stay that way -- no
assertions, no SystemVerilog constructs, nothing formal-specific, ever,
inside `rtl/`. All formal content lives in `hdl/formal/` and may use
SystemVerilog, but only a restricted subset ("SVA-lite"):

- Plain immediate `assert (...)` / `assume (...)` / `cover (...)`
  statements inside `always @(posedge clk)` blocks.
- No `assert property (...)`, no sequence/property operators (`|->`,
  `##N`, etc.), no `$past()`.

This isn't a stylistic preference -- the open-source Yosys formal flow
(no Verific plugin available in this environment) only reliably supports
the immediate-assertion subset. Full SVA sequence syntax either fails to
parse or silently doesn't do what it looks like it does. Anything that
needs "N cycles ago" state should be built by hand with a `prev_*` shadow
register updated every clock, exactly like an RTL designer would build a
delay line -- which is also easier to audit than a property-language
temporal operator.

## White-box checks: flatten + `expose`, not `bind`, not a dotted reference

A checker that needs to read a DUT's internal (non-port) signal cannot
reach it directly in this Yosys build. Two approaches were tried and
**both silently fail to connect**, rather than erroring:

1. **A dotted hierarchical reference** (`wire x = dut.internal_signal;`
   from the top-level testbench). Yosys's `read_verilog -sv` does not
   resolve `dut.internal_signal` as a hierarchical reference into an
   already-elaborated child instance -- it silently declares a new,
   disconnected, free-valued top-level wire literally named
   `dut.internal_signal` (visible as an "implicitly declared" warning
   during `read_verilog`, easy to miss). Any "assertion failure" is the
   solver exploiting that phantom free signal, not a real DUT bug.
2. **SystemVerilog `bind`** (a separate `_checks.sv` module attached via
   `bind foo foo_checks u_checks ();`). This *looks* like the right fix
   for (1) -- no dotted names, references resolve as if textually placed
   inside the target module -- but this Yosys build's frontend accepts
   the `bind` syntax without error and then never actually instantiates
   the bound module: `hierarchy`'s dependency analysis reports
   "Removing unused module `foo_checks`", and the bound checker's
   asserts/covers never make it into the design at all. **This is worse
   than (1), not better**: there is no implicit-declaration warning or
   any other visible sign anything is wrong -- a proof built this way
   reports PASS unconditionally, because it is silently checking nothing.
   Confirmed by injecting a deliberately-false `assert (1'b0)` into a
   `bind`-attached checker and finding the `prove` task still passed.
   (Full `bind` support requires Yosys's commercial Verific frontend,
   not available here.)

**Use a two-stage `flatten` + `expose` flow instead**, verified to
actually connect (confirmed the reverse way: a deliberately-false
assertion on an exposed probe reliably fails the proof at the expected
step). Stage 1 elaborates a trivial single-instance wrapper around the
DUT (e.g. `foo_bare.v`, just `foo dut (...);`), flattens it, and uses
Yosys's `expose` pass to promote the specific internal registers a
checker needs into ordinary output ports (named `\dut.<signal>` by
`expose`'s default separator); stage 2 resets the design and
re-elaborates from the now-exposed generated module plus the actual
formal harness top, which instantiates it and asserts/covers directly
against the exposed probes via plain named port connections -- no
`bind`, no dotted references, just ordinary Verilog instantiation of a
module that now genuinely has those signals as ports. Both stages live
in the same `.sby` `[script]` block (see `cordic.sby` or
`axi4s_skid_buffer.sby` for the full pattern):

```
read_verilog -formal -I. foo.v
read_verilog -formal -I. foo_bare.v
hierarchy -top foo_bare
proc
flatten
expose -dff w:\dut.internal_signal
write_verilog gen_foo_bare.v

design -reset
read_verilog -formal -I. gen_foo_bare.v
read_verilog -formal -I. foo_formal.v
prep -top foo_formal
```

`foo_formal.v` then instantiates the generated `foo_bare` (module name
is unchanged by `expose`, only its port list grows) and connects the new
port via its escaped Verilog name, with a space before the parenthesis
(escaped identifiers end at whitespace, not at `(`):

```systemverilog
wire internal_probe;
foo_bare dut_top (
    .clk(clk), .rst(rst), /* ...other ports... */
    .\dut.internal_signal (internal_probe)
);
always @(posedge clk) assert (internal_probe == expected);
```

One elaboration wrinkle: if the DUT module being wrapped is
parameterized (e.g. `DATA_WIDTH`), `flatten` specializes and *removes*
that parameter from the generated module -- don't pass a
`#(.DATA_WIDTH(...))` override when instantiating the generated module
in stage 2, its ports are already fixed at whatever width stage 1 used.

## Small-parameterization convention

Formal proofs run at a small, fixed data width (4 bits has been
sufficient so far), never at a block's real operating width (16-bit
Q1.15, 8192-entry FFT, etc.). The properties being checked are protocol
and control-flow invariants (no data loss, no stall violation, reset
behavior, FSM legality) that don't depend on width -- proving them at
width 4 is exhaustive over the *interesting* state space and keeps BMC/
induction fast. Bit-exact numerical correctness is cocotb's job (against
the real golden model, at the real width), not formal's.

## `mode prove`, not just `mode bmc`

Prefer SymbiYosys `mode prove` (BMC + k-induction) over bare `mode bmc`
wherever it closes -- it proves the property holds for all time, not just
within the depth searched. Bounded `mode bmc` is a fallback for
properties k-induction can't close without additional invariants (not yet
needed for anything built so far). Pair every `prove` task with a
`cover` task for the properties' antecedents (see
`axi4s_skid_buffer.sby`'s `[tasks] prove / cover` split) -- an assertion
that only ever holds because its guarding condition is unreachable proves
nothing.

## Solver

Qualified with `hdl/formal/qualify/qualify_solvers.sh` (a trivial
passing and a trivial failing harness per engine, 60 s each; a crash, a
missing status or a timeout disqualifies):

| Engine | Result |
|---|---|
| `smtbmc z3` | qualified; default for every smtbmc harness |
| `smtbmc cvc5` | qualified, but timed out (900 s) on cordic, cp_removal and fft_engine where z3 takes 6-340 s |
| `smtbmc boolector` | unusable (broken pipe, no status) |
| `smtbmc yices`, `smtbmc bitwuzla` | not installed |
| sby `abc pdr` / `abc bmc3` | unusable through sby (`KeyError: 'asserts'`, see below) |
| ABC `pdr` / `bmc3` on the sby-built AIG | qualified; this is `prove_pdr.sh` |

Always name the solver in the `.sby` engine line (`smtbmc z3`); bare
`smtbmc` tries yices first. Re-run the qualification after any toolchain
change.

## Datapath-heavy blocks: cut multipliers, prove with PDR

Blocks with wide arithmetic (`bootstrap_detector`: 64-bit dividers and
accumulators, several multipliers) stall `smtbmc z3` outright -- it could
not finish a depth-2 BMC there. Two measures, both used by
`bootstrap_detector.sby`:

- **`cutpoint t:$mul`** after `prep`: every multiplier output becomes a
  fresh free value each cycle. Sound for control/protocol properties (if a
  property holds for arbitrary products it holds for the real ones);
  don't use it for a property that depends on a product's value.
- **ABC PDR** (`abc pdr`, bit-level, unbounded) instead of k-induction.
  With the multipliers cut, it proved the bootstrap detector's properties
  in under a second. Run it through `hdl/formal/prove_pdr.sh <job>`, not
  `sby` directly: SBY 0.68's ABC result parser expects an `asserts` key in
  the witness map that Yosys 0.33's `write_aiger` doesn't emit, so `sby`
  crashes (`KeyError: 'asserts'`) after PDR finishes. The script still
  uses the `.sby` file to build the model, and checks that stage
  succeeded before running PDR itself.

Neither ABC PDR nor `smtbmc` gives a practical `cover` task on these
models, so non-vacuity has to come from **RTL mutants** instead, which
are committed in `hdl/mutants/manifest.py` and rerun with
`hdl/mutants/run_mutants.py` (a `bmc:<job>` checker runs
`prove_pdr.sh --mutant <job>`, bounded `bmc3`). Don't hunt mutants with PDR: it is a
proof engine and stalls on deep counterexamples -- a bootstrap detector
mutant needing ~160 frames (two samples through the dividers) ran PDR for
14 hours without a verdict, while `bmc3` found it in under two minutes.
Both modes of `prove_pdr.sh` run under a time limit (`PDR_TIMEOUT`,
`BMC_FRAMES`/`BMC_TIMEOUT`) and report exhausting it as UNDECIDED (exit
2), never as a pass. Also keep in mind that `prev_*` shadow
registers sample the DUT's uninitialized outputs on the reset cycle: gate
any `prev_*`-based assertion on `!prev_rst`. PDR found exactly that bug
in the first version of this harness.

## `.sby` file mechanics

`[files]` entries get flattened into one `src/` directory regardless of
their source subdirectory, so `[script]` `read_verilog` calls must
reference bare filenames (with `-I.` for includes), not the relative
paths used in `[files]`:

```
[script]
read_verilog -formal -I. axi4s_skid_buffer.v

[files]
axi4s_skid_buffer.v ../rtl/common/axi4s_skid_buffer.v
```

## Running, and work directories

`hdl/formal/run_formal.sh [job ...]` runs every harness under a per-job
time limit and prints PASS/FAIL per job. Work directories and logs go to
`hdl/build/formal/` (gitignored); nothing is written next to the
`.sby` files.

## Mutants

Every harness has at least one committed mutant
(`hdl/mutants/manifest.py`): a one-line RTL change its properties must
reject. `run_mutants.py` applies each to a private copy of `hdl/rtl/`
and reports KILLED / SURVIVED / STALE / ERROR per checker. A formal
checker can also report "KILLED (induction only)": the mutant makes
k-induction stop closing but BMC finds no counterexample within the
harness depth because the bug sits behind deeper events. That still
shows the properties depend on the mutated logic, and is labelled so a
reader knows no trace was produced.
