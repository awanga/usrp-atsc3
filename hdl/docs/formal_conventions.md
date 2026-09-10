# Formal Verification Conventions

> Established and verified against
> `hdl/rtl/common/axi4s_skid_buffer.v` / `hdl/formal/axi4s_skid_buffer.sby`
> before any algorithmic block's formal harness was written. Follow this
> pattern for every subsequent block's formal work.

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

`z3` (also available: `boolector`). `yices-smt2` is **not** installed in
this environment -- don't default `.sby` engine lines to bare `smtbmc`
(which tries yices first and fails with "not found in path"); always
name a solver explicitly: `smtbmc z3`.

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

## Work directories

`sby` creates a directory per task (named `<sby-basename>_<task>/`, e.g.
`axi4s_skid_buffer_prove/`) containing the full proof database and any
counterexample traces. These are gitignored (`hdl/formal/*/` in
`.gitignore`) -- regenerate by re-running `sby -f <file>.sby`, don't
commit them.
