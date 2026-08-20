# Formal Verification Conventions

> Phase 9.0 deliverable. Established and verified against
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

## White-box checks: use `bind`, never a dotted hierarchical reference

A checker that needs to read a DUT's internal (non-port) signal must be
attached via SystemVerilog `bind`, in a separate `_checks.sv` file plus a
one-line `_bind.sv` file:

```systemverilog
// _checks.sv -- no ports; references clk, rst, and the DUT's internal
// signals by bare name, resolved by bind's scoping rules.
module foo_checks;
    always @(posedge clk) begin
        if (some_condition) assert (internal_signal == expected);
    end
endmodule
```
```systemverilog
// _bind.sv
bind foo foo_checks u_checks ();
```

**Do not** write `wire x = dut.internal_signal;` from the top-level
testbench and assert on `x`. This was tried first for the skid buffer and
produced a real, confusing failure: Yosys's `read_verilog -sv` does not
resolve `dut.internal_signal` as a hierarchical reference into an
already-elaborated child instance at parse time -- it silently declares a
new, disconnected, free-valued top-level wire literally named
`dut.internal_signal` (visible as an "implicitly declared" warning during
`read_verilog`, easy to miss). The resulting "assertion failure" was the
solver exploiting that phantom free signal, not a real DUT bug. `bind`
avoids this because a bound instance is elaborated as if textually placed
inside the target module, so its unqualified references resolve as true
internal signals, not dotted paths.

If a checker's own declarations need the target's parameter value (e.g.
a data-width-sized register), don't try to reference the target's
parameter by name in the checker's declarations -- Yosys resolves a bound
module's own declarations before attaching it to the target scope, so
parameter values aren't visible yet at that point. Give the checker its
own local constant instead (see `axi4s_skid_buffer_checks.sv`'s
`CHK_DATA_WIDTH`). This is fine in practice because of the next
convention:

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
