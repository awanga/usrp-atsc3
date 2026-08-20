// axi4s_types.vh — Shared AXI4-Stream constants and port-list macros
//
// IEEE 1364-2001 Verilog only. Include at the top of any module that needs
// these constants or macros:
//   `include "axi4s_types.vh"
//
// Supersedes hdl/stubs/axi4s_interface.vh for all rtl/ blocks. That file
// declared its widths with top-level `parameter`, which is not legal
// outside a module body in 1364-2001 -- it only ever worked because it was
// never actually compiled standalone. This file uses `define instead, which
// is legal at file scope and expands correctly wherever it's included.
//
// hdl/stubs/ is kept only for its README/spec value (see hdl/docs); no rtl/
// module should `include the old stub header.

`ifndef AXI4S_TYPES_VH
`define AXI4S_TYPES_VH

//------------------------------------------------------------------------------
// Sample width. RTL is fixed-point-only (Decision: RTL ports the
// ATSC3_FIXED_POINT=ON build; see hdl/docs/q_format_notes.md). This is a
// deliberate RTL-vs-golden-model-doc divergence: most lib/ headers document
// cf32 in their AXI4-S comments because the float build is the default C++
// config, but RTL always uses ci16 (Q1.15).
//------------------------------------------------------------------------------

`define ATSC3_REAL_WIDTH   16                                  // Q1.15, one rail
`define ATSC3_SAMPLE_WIDTH (2 * `ATSC3_REAL_WIDTH)              // {im[15:0], re[15:0]}

// LLR width (soft bits from demapper into LDPC)
`define ATSC3_LLR_WIDTH 8

// Decoded / corrected bit width
`define ATSC3_BIT_WIDTH 1

//------------------------------------------------------------------------------
// AXI4-Stream port-list macros
//
// Expand inside a module's port list. Trailing comma is the caller's
// responsibility (these do not emit one), matching normal Verilog port-list
// style, e.g.:
//
//   module foo (
//       input  wire clk,
//       input  wire rst,
//       `AXI4S_SLAVE(s_axis, `ATSC3_SAMPLE_WIDTH),
//       `AXI4S_MASTER(m_axis, `ATSC3_SAMPLE_WIDTH)
//   );
//------------------------------------------------------------------------------

`define AXI4S_MASTER(prefix, data_width) \
    output reg  [(data_width)-1:0] prefix``_tdata, \
    output reg                     prefix``_tvalid, \
    input  wire                    prefix``_tready, \
    output reg                     prefix``_tlast

`define AXI4S_SLAVE(prefix, data_width) \
    input  wire [(data_width)-1:0] prefix``_tdata, \
    input  wire                    prefix``_tvalid, \
    output reg                     prefix``_tready, \
    input  wire                    prefix``_tlast

// Transfer fires when both sides of one specific interface assert
`define AXI4S_FIRE(prefix) ((prefix``_tvalid) && (prefix``_tready))

//------------------------------------------------------------------------------
// FFT sizes (bootstrap is always 4K regardless of data FFT size --
// see CLAUDE.md Common Pitfalls)
//------------------------------------------------------------------------------

`define ATSC3_FFT_4K       4096
`define ATSC3_FFT_8K       8192
`define ATSC3_FFT_16K      16384
`define ATSC3_FFT_32K      32768

`define ATSC3_FFT_4K_LOG2  12
`define ATSC3_FFT_8K_LOG2  13
`define ATSC3_FFT_16K_LOG2 14
`define ATSC3_FFT_32K_LOG2 15

//------------------------------------------------------------------------------
// LDPC codeword lengths
//------------------------------------------------------------------------------

`define ATSC3_LDPC_SHORT 16200
`define ATSC3_LDPC_LONG  64800

//------------------------------------------------------------------------------
// Nominal clock (technology-independent target; see hdl/synth metrics,
// Decision 4 -- not a synthesis constraint on its own)
//------------------------------------------------------------------------------

`define ATSC3_CLK_PERIOD_NS 10  // 100 MHz nominal

`endif // AXI4S_TYPES_VH
