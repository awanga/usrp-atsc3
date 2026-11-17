// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// frame_sync_gen.cc — retired golden-vector CLI for hdl/rtl/sync/frame_sync*.v
// and frame_correlator.v
//
// Those modules port the old FrameSync, which correlated against an
// invented preamble reference. FrameSync now tracks frame boundaries from
// validated A/321 bootstraps (lib/sync/frame_sync.h) and has no sample
// data path, so there is no golden output for the old RTL protocol. This
// CLI stays as a build target so a stale binary cannot keep the old
// cocotb benches passing: it always fails.

#include <iostream>

int main() {
    std::cerr << "frame_sync_gen: retired -- hdl/rtl/sync/frame_sync*.v and frame_correlator.v "
                 "model a preamble correlation that lib/sync/frame_sync no longer has; "
                 "see TASKS.md\n";
    return 2;
}
